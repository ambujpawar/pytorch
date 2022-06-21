#pragma once
#include <ATen/record_function.h>
#include <c10/util/Optional.h>
#include <torch/custom_class.h>

namespace torch {
namespace autograd {
namespace profiler {

struct PythonRecordFunction : public torch::CustomClassHolder {
  at::RecordFunction record;

  explicit PythonRecordFunction(
      at::RecordScope scope = at::RecordScope::FUNCTION)
      : record(scope) {}
};

// Creates a new profiling scope using RecordFunction and invokes its starting
// callbacks.
TORCH_API c10::intrusive_ptr<PythonRecordFunction> record_function_enter_new(
    const std::string& name,
    const c10::optional<std::string>& args = c10::nullopt);

// Legacy signature using cpp_custom_type_hack
TORCH_API at::Tensor record_function_enter_legacy(
    c10::string_view name,
    c10::optional<c10::string_view> args);
TORCH_API void record_function_exit_legacy(const at::Tensor& handle);

// Schedules RecordFunction's end callbacks to be run on completion of a future.
TORCH_API c10::intrusive_ptr<c10::ivalue::Future> _call_end_callbacks_on_fut_new(
    const c10::intrusive_ptr<PythonRecordFunction>& record,
    const c10::intrusive_ptr<c10::ivalue::Future>& fut);

} // namespace profiler
} // namespace autograd
} // namespace torch
