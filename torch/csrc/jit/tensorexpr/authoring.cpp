#include <pybind11/functional.h>
#include <pybind11/operators.h>
#include <pybind11/stl.h>
#include <torch/csrc/jit/python/pybind_utils.h>
#include <torch/csrc/jit/tensorexpr/codegen.h>
#include <torch/csrc/jit/tensorexpr/ir_printer.h>
#include <torch/csrc/jit/tensorexpr/ir_simplifier.h>
#include <torch/csrc/jit/tensorexpr/kernel.h>
#include <torch/csrc/jit/tensorexpr/llvm_codegen.h>
#include <torch/csrc/jit/tensorexpr/loopnest.h>
#include <torch/csrc/jit/tensorexpr/reduction.h>
#include <array>
#include <cassert>
#include <map>
#include <mutex>

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define AT __FILE__ ":" TOSTRING(__LINE__)
#define AA(test) \
  if (!(test))   \
  throw std::runtime_error("assert failed " AT)

namespace torch {
namespace jit {
namespace {
using namespace torch::jit::tensorexpr;

template <int MAX_DIMS>
class SpecializationKey {
 protected:
  enum DimFlags {
    SIZE_MISSING = 1 << 0, // leading dimension implicitly added
    SIZE_ONE = 1 << 1, // == 1
    SIZE_OTHER = 1 << 2, // > 1

    STRIDE_ZERO = 1 << 3, // == 0 (broadcast)
    STRIDE_ONE = 1 << 4, // == 1 (packed)
    STRIDE_CONTIGUOUS = 1 << 5, // stride[i+1] * sizes[i+1]
    STRIDE_TRANSPOSED_CONTIGUOUS = 1 << 6, // stride[i-1] * sizes[i-1]
    STRIDE_AS_ARG = 1 << 7,
  };
  static constexpr int MASK = (1 << 5) - 1;

  static inline uint16_t pack_flags(const at::Tensor& v, bool is_out) {
    // pack all the tensor properties into a uint16 for fast hash/compare
    constexpr uint16_t S0 = 1;
    constexpr uint16_t S1 = S0 * 2;
    constexpr uint16_t S2 = S1 * 2;
    constexpr uint16_t S3 = S2 * static_cast<int>(at::ScalarType::NumOptions);
    constexpr uint16_t S4 = S3 * static_cast<int>(at::Layout::NumOptions);
    constexpr uint16_t S5 =
        S4 * static_cast<int>(at::DeviceType::COMPILE_TIME_MAX_DEVICE_TYPES);
    static_assert(S3 < S4 && S4 < S5); // overflow check

    at::ScalarType dtype = v.dtype().toScalarType();
    at::DeviceType device = v.device().type();
    at::Layout layout = v.layout();
    bool requires_grad = v.requires_grad();

    return S0 * static_cast<uint16_t>(is_out) +
        S1 * static_cast<uint16_t>(requires_grad) +
        S2 * static_cast<uint16_t>(dtype) + S3 * static_cast<uint16_t>(layout) +
        S4 * static_cast<uint16_t>(device);
  }

  template <typename T>
  inline void init_dimflags(const T& sizes, const T& strides, int64_t ndims) {
    // pack all the properties for each dimension into a uint8
    int out_idx = 0;
    for (int dim = 0; dim < ndims; ++dim) {
      uint8_t flag = (sizes[dim] == 1 ? SIZE_ONE : SIZE_OTHER);
      if (strides[dim] == 0)
        flag |= STRIDE_ZERO;
      else if (strides[dim] == 1)
        flag |= STRIDE_ONE;
      else if (
          dim + 1 < sizes.size() &&
          strides[dim] == strides[dim + 1] * sizes[dim + 1])
        flag |= STRIDE_CONTIGUOUS;
      else if (dim > 0 && strides[dim] == strides[dim - 1] * sizes[dim - 1])
        flag |= STRIDE_TRANSPOSED_CONTIGUOUS;
      else
        flag |= STRIDE_AS_ARG;
      dimflags_[out_idx++] = flag;
    }
    while (out_idx < MAX_DIMS) {
      dimflags_[out_idx++] = SIZE_MISSING | STRIDE_ZERO;
    }
  }

 public:
  SpecializationKey() {}

  SpecializationKey(const at::Tensor& v, int8_t alias_group, bool is_out)
      : flags_(pack_flags(v, is_out)), alias_group_(alias_group) {
    init_dimflags(v.sizes(), v.strides(), v.ndimension());
  }

  int cmp(const SpecializationKey<MAX_DIMS>& other) const {
    return memcmp(
        &flags_,
        &other.flags_,
        sizeof(flags_) + sizeof(alias_group_) + sizeof(dimflags_));
  }

  std::vector<std::string> shape() const {
    std::vector<std::string> result;
    for (int i = 0; i < MAX_DIMS; ++i) {
      if ((dimflags_[i] & SIZE_MISSING) > 0)
        break;

      if ((dimflags_[i] & SIZE_ONE) > 0)
        result.push_back("one");
      else
        result.push_back("other");
    }
    return result;
  }
  std::vector<std::string> stride() const {
    std::vector<std::string> result;
    for (int i = 0; i < MAX_DIMS; ++i) {
      if ((dimflags_[i] & SIZE_MISSING) > 0)
        break;

      if ((dimflags_[i] & STRIDE_ZERO) > 0)
        result.push_back("zero");
      else if ((dimflags_[i] & STRIDE_ONE) > 0)
        result.push_back("one");
      else if ((dimflags_[i] & STRIDE_CONTIGUOUS) > 0)
        result.push_back("contiguous");
      else if ((dimflags_[i] & STRIDE_TRANSPOSED_CONTIGUOUS) > 0)
        result.push_back("transposed_contiguous");
      else if ((dimflags_[i] & STRIDE_AS_ARG) > 0)
        result.push_back("as_arg");
      else
        throw std::runtime_error("??");
    }
    return result;
  }

  py::object to_python(const at::Tensor& example) const {
    py::object ex = py::cast(example);
    py::object namedtuple =
        py::module_::import("collections").attr("namedtuple");
    py::object rtype = namedtuple(
        "SpecializationKey",
        "alias_group,ndim,dtype,device,layout,requires_grad,out,shape,stride");
    return rtype(
        static_cast<int>(alias_group_),
        ex.attr("ndim"),
        ex.attr("dtype"),
        ex.attr("device"),
        ex.attr("layout"),
        ex.attr("requires_grad"),
        py::bool_(flags_ % 2),
        shape(),
        stride());
  }

 private:
  uint16_t flags_; // dtype, layout, device, and requires_grad
  int8_t alias_group_; // 0 = no aliasing
                       // >0 = same data, strides, and shapes within group
                       // <0 = overlapping storage madness
  uint8_t dimflags_[MAX_DIMS];
} __attribute__((packed));

class CompileResultBase : public KernelScopedObject {
 public:
  virtual ~CompileResultBase() = default;
  virtual void set_code(const py::object& cg) = 0;
  virtual void set_shape_from(const std::vector<std::pair<int, int>>& indices) = 0;
  virtual void set_options_from(int index) = 0;
  virtual void add_shape_check(const std::tuple<int, int, int, int>& indices) = 0;
};

struct CompileResultProxy {
  CompileResultBase* res;
  explicit CompileResultProxy(CompileResultBase* r) : res(r) {}
};

struct CmpLess {
  template <typename T>
  size_t operator()(const T& left, const T& right) const {
    for (int i = 0; i < left.size(); ++i) {
      auto c = left[i].cmp(right[i]);
      if (c < 0)
        return true;
      if (c > 0)
        return false;
    }
    return false;
  }
};

template <int NARGS, int MAX_DIMS>
class CompileCache3 {
 public:
  typedef SpecializationKey<MAX_DIMS> ArgKey;
  typedef std::array<ArgKey, NARGS> Key;
  typedef std::array<at::Tensor, NARGS> Args;
  typedef std::array<int8_t, NARGS> AliasGroups;

  class CompileResultImpl : public CompileResultBase {
   public:
    void set_code(const py::object& cg) {
      objects_.push_back(cg);
      cg_ = cg.cast<CodeGen*>();
    }
    void set_shape_from(const std::vector<std::pair<int, int>>& indices) {
      assert(indices.shape() <= MAX_DIMS);
      shape_from_ = indices;
    }
    void set_options_from(int index) {
      options_from_ = index;
    }

    void add_shape_check(const std::tuple<int, int, int, int>& indices){
        shape_checks_.push_back(indices);
    }

    at::Tensor call(const Args& args, std::vector<void*>& call_args) {
      int64_t shapes[MAX_DIMS];
      for (int i = 0; i < shape_from_.size(); ++i) {
        shapes[i] = args[shape_from_[i].first].size(shape_from_[i].second);
        call_args.emplace_back(shapes + i);
      }
      cg_->call_raw(call_args);
      return args[2];
    }

   private:
    CodeGen* cg_;
    std::vector<std::pair<int, int>> shape_from_;
    std::vector<std::tuple<int, int, int, int>> shape_checks_;
    int options_from_ = 0;
    std::vector<py::object> objects_; // for ref counting
  };
  typedef std::map<Key, CompileResultImpl*, CmpLess> Map;

  CompileResultImpl* cached_compile(const Key& key, const Args& example) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto item = cache_.find(key);
    if (item != cache_.end()) {
      return item->second;
    } else {
      auto cr = new CompileResultImpl();
      std::vector<py::object> spec;
      for (int i = 0; i < key.size(); ++i) {
        spec.push_back(key[i].to_python(example[i]));
      }
      compile_fn_(spec, CompileResultProxy(cr));
      cache_.emplace(std::make_pair(key, cr));
      return cr;
    }
  }

  int8_t aliasing_check(const at::Tensor& a, const at::Tensor& b) {
    if (a.is_alias_of(b)) {
      if (a.is_set_to(b)) {
        return 1;
      } else {
        // TODO: check for non-overlapping and return 0
        //       likely we could lift some logic from tensoriterator
        return -1;
      }
    } else {
      return 0;
    }
  }

  AliasGroups compute_alias_groups(const Args& args) {
    AliasGroups alias_groups;
    int8_t current_id = 0;
    for (int i = 0; i < NARGS; ++i) {
      alias_groups[i] = 0;
    }
    for (int i = 0; i < NARGS; ++i) {
      if (alias_groups[i] == 0) {
        for (int j = i + 1; j < NARGS; ++j) {
          int8_t alias_type = aliasing_check(args[i], args[j]);
          if (alias_type != 0) {
            if (alias_groups[i] == 0)
              ++current_id;
            alias_groups[i] = current_id;
            alias_groups[j] = current_id * alias_type;
          }
        }
      }
    }
    return alias_groups;
  }

  Key compute_cache_key(const Args& args, bool has_out) {
    AliasGroups alias_groups = compute_alias_groups(args);
    Key key;
    int i = 0;
    for (; i < NARGS - 1; ++i) {
      key[i] = ArgKey(args[i], alias_groups[i], false);
    }
    if (i < NARGS) {
      key[i] = ArgKey(args[i], alias_groups[i], has_out);
    }
    return key;
  }

  CompileCache3(const py::object& compile_fn) : compile_fn_(compile_fn) {}

  at::Tensor call(const Args& args, bool has_out) {
    std::vector<void*> call_args;
    call_args.reserve(NARGS + NARGS * MAX_DIMS);
    for (const auto& arg : args) {
      call_args.emplace_back(arg.data_ptr());
    }
    auto key = compute_cache_key(args, has_out);
    return cached_compile(key, args)->call(args, call_args);
    /*
    int64_t n = a.sizes()[0];
    int64_t shapes[] = {n};
    int64_t strides[] = {1};
    at::Tensor out = at::empty_strided(shapes, strides);
    std::vector<void*> args = {a.data_ptr(), b.data_ptr(), out.data_ptr(), &n};
    self.call_raw(args);
    */
  }

 public:
  std::mutex mutex_;
  Map cache_;
  py::object compile_fn_;
};

template <int NARGS>
class CompileCache2 {
 public:
  CompileCache2(const py::object& compile_fn)
      : cache2(compile_fn), cache4(compile_fn), cache8(compile_fn) {}

  at::Tensor call(const std::array<at::Tensor, NARGS>& args, bool has_out) {
    // fan out and and specialize on number of dimension buckets
    int64_t ndims = 0;
    for (const auto& item : args) {
      ndims = std::max(item.ndimension(), ndims);
    }
    if (ndims <= 2)
      return cache2.call(args, has_out);
    if (ndims <= 4)
      return cache4.call(args, has_out);
    if (ndims <= 8)
      return cache8.call(args, has_out);
    throw std::runtime_error("TODO: handle more dims");
  }

 private:
  CompileCache3<NARGS, 2> cache2;
  CompileCache3<NARGS, 4> cache4;
  CompileCache3<NARGS, 8> cache8;
};

class CompileCache {
 public:
  CompileCache(const py::object& compile_fn)
      : cache1(compile_fn),
        cache2(compile_fn),
        cache3(compile_fn),
        cache4(compile_fn) {}

  at::Tensor call(py::args args, py::kwargs kwargs) {
    // fan out an specialize on arg counts
    int num_args = py::len(args);
    int num_kwargs = py::len(kwargs);
    bool has_out = num_kwargs == 1;
    at::Tensor last_arg;

    if (num_kwargs == 1) {
      last_arg = kwargs["out"].cast<at::Tensor>();
    } else if (num_kwargs > 1) {
      throw std::runtime_error("expected at most 1 kwarg");
    } else {
      last_arg = args[num_args - 1].cast<at::Tensor>();
    }

    switch (num_args + num_kwargs) {
      case 1:
        return cache1.call(std::array<at::Tensor, 1>{last_arg}, has_out);
      case 2:
        return cache2.call(
            std::array<at::Tensor, 2>{
                args[0].cast<at::Tensor>(),
                last_arg,
            },
            has_out);
      case 3:
        return cache3.call(
            std::array<at::Tensor, 3>{
                args[0].cast<at::Tensor>(),
                args[1].cast<at::Tensor>(),
                last_arg,
            },
            has_out);
      case 4:
        return cache4.call(
            std::array<at::Tensor, 4>{
                args[0].cast<at::Tensor>(),
                args[1].cast<at::Tensor>(),
                args[2].cast<at::Tensor>(),
                last_arg,
            },
            has_out);

      default:
        throw std::runtime_error("TODO: handle other arg counts");
    }
  }

 private:
  CompileCache2<1> cache1;
  CompileCache2<2> cache2;
  CompileCache2<3> cache3;
  CompileCache2<4> cache4;
};

} // namespace

void initTensorExprAuthoringBindings(PyObject* te_obj) {
  py::handle te(te_obj);

  py::class_<CompileCache>(te, "CompileCache")
      .def(py::init<py::object>())
      .def("__call__", &CompileCache::call);

  py::class_<CompileResultProxy>(te, "CompileResult")
      .def(
          "set_code",
          [](CompileResultProxy& self, const py::object& cg) {
            self.res->set_code(cg);
          })
      .def(
          "add_shape_check",
          [](CompileResultProxy& self, const std::tuple<int, int, int, int>& indices) {
            self.res->add_shape_check(indices);
          })
      .def(
          "set_shape_from",
          [](CompileResultProxy& self, const std::vector<std::pair<int, int>>& indices) {
            self.res->set_shape_from(indices);
          })
      .def("set_options_from", [](CompileResultProxy& self, int index) {
        self.res->set_options_from(index);
      });
}
} // namespace jit
} // namespace torch
