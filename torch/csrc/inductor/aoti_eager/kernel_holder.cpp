#if !defined(C10_MOBILE) && !defined(ANDROID)
#include <torch/csrc/inductor/aoti_eager/kernel_holder.h>

#include <ATen/ATen.h>

#include <ATen/core/dispatch/Dispatcher.h>
#include <torch/csrc/PyInterpreter.h>
#include <torch/csrc/autograd/python_variable.h>
#include <torch/csrc/inductor/aoti_runner/model_container_runner_cpu.h>
#ifdef USE_CUDA
#include <torch/csrc/inductor/aoti_runner/model_container_runner_cuda.h>
#endif
#include <torch/csrc/jit/frontend/function_schema_parser.h>

#include <ATen/core/jit_type.h>
#include <torch/csrc/inductor/aoti_runner/model_container_runner_cpu.h>
#include <torch/csrc/inductor/aoti_torch/c/shim.h>
#include <torch/csrc/inductor/aoti_torch/tensor_converter.h>

namespace torch::inductor {

namespace {

c10::ScalarType parse_dtype(const std::string& dtype_str) {
  // The dtype format is torch.float32, float32, torch.int32, int32, etc.
  std::string to_remove = "torch.";
  std::string canonicalized_dtype_str = dtype_str;
  size_t start_pos = dtype_str.find(to_remove);
  if (start_pos != std::string::npos) {
    canonicalized_dtype_str = dtype_str.substr(start_pos + to_remove.length());
  }

  if (canonicalized_dtype_str == "float32") {
    return c10::ScalarType::Float;
  } else if (canonicalized_dtype_str == "int32") {
    return c10::ScalarType::Int;
  } else if (canonicalized_dtype_str == "int64") {
    return c10::ScalarType::Long;
  } else if (canonicalized_dtype_str == "bool") {
    return c10::ScalarType::Bool;
  } else if (canonicalized_dtype_str == "bfloat16") {
    return c10::ScalarType::BFloat16;
  } else if (canonicalized_dtype_str == "float16") {
    return c10::ScalarType::Half;
  } else if (canonicalized_dtype_str == "float64") {
    return c10::ScalarType::Double;
  } else if (canonicalized_dtype_str == "uint8") {
    return c10::ScalarType::Byte;
  } else if (canonicalized_dtype_str == "int8") {
    return c10::ScalarType::Char;
  } else if (canonicalized_dtype_str == "complex64") {
    return c10::ScalarType::ComplexFloat;
  } else if (canonicalized_dtype_str == "complex128") {
    return c10::ScalarType::ComplexDouble;
  } else {
    TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
        false, "Unsupported dtype: ", canonicalized_dtype_str);
    return c10::ScalarType::Undefined;
  }
}

inline void unpack_tensor_ivalue(
    const c10::IValue& ivalue,
    const c10::Device& device,
    std::vector<at::Tensor>& inputs) {
  inputs.push_back(ivalue.toTensor());
}

inline void unpack_optional_tensor_ivalue(
    const c10::IValue& ivalue,
    const c10::Device& device,
    std::vector<at::Tensor>& inputs) {
  auto ivalue_opt_tensor = ivalue.toOptional<at::Tensor>();
  if (ivalue_opt_tensor.has_value()) {
    inputs.push_back(ivalue_opt_tensor.value());
  }
}

inline void unpack_tensor_list_ivalue(
    const c10::IValue& ivalue,
    const c10::Device& device,
    std::vector<at::Tensor>& inputs) {
  for (const auto& item : ivalue.toListRef()) {
    inputs.push_back(item.toTensor());
  }
}

inline void unpack_optional_tensor_list_ivalue(
    const c10::IValue& ivalue,
    const c10::Device& device,
    std::vector<at::Tensor>& inputs) {
  for (const auto& item : ivalue.toListRef()) {
    unpack_optional_tensor_ivalue(item, device, inputs);
  }
}

inline void unpack_scalar_ivalue(
    const c10::IValue& ivalue,
    const c10::Device& device,
    std::vector<at::Tensor>& inputs) {
  inputs.push_back(at::scalar_tensor(
      ivalue.toScalar(),
      c10::TensorOptions().device(device).dtype(ivalue.toScalar().type())));
}

bool unpack_ivalue(
    const c10::Argument& argument,
    const c10::IValue& ivalue,
    const c10::Device& device,
    std::vector<at::Tensor>& inputs) {
  if (ivalue.isTensor()) {
    unpack_tensor_ivalue(ivalue, device, inputs);
  } else if (ivalue.isTensorList()) {
    unpack_tensor_list_ivalue(ivalue, device, inputs);
  } else if (ivalue.isOptionalTensorList()) {
    unpack_optional_tensor_list_ivalue(ivalue, device, inputs);
  } else if (ivalue.isScalar()) {
    // ivalue is scalar
    unpack_scalar_ivalue(ivalue, device, inputs);
  } else if (
      *argument.real_type() == *c10::getTypePtr<c10::optional<at::Tensor>>()) {
    // ivalue is c10::optional<at::Tensor>
    unpack_optional_tensor_ivalue(ivalue, device, inputs);
  } else {
    // Unsupport IValue type.
    return false;
  }

  return true;
}

bool unpack_tensors(
    const std::vector<c10::Argument>& arguments,
    const torch::jit::Stack& stack,
    const c10::Device& device,
    std::vector<at::Tensor>& inputs,
    bool with_scalar = false) {
  for (size_t idx = 0; idx < stack.size(); idx++) {
    if (!with_scalar && stack[idx].isScalar()) {
      continue;
    }

    if (!unpack_ivalue(arguments[idx], stack[idx], device, inputs)) {
      return false;
    }
  }

  return true;
}

std::vector<size_t> get_tensor_paramter_index(
    const std::vector<c10::Argument>& arguments,
    const torch::jit::Stack& stack) {
  std::vector<size_t> tensor_parameter_index;
  for (size_t idx = 0; idx < stack.size(); idx++) {
    if (stack[idx].isScalar() || stack[idx].isTensor()) {
      // scalar and tensor
      tensor_parameter_index.push_back(idx);
    } else if (stack[idx].isTensorList()) {
      // tensor list
      std::fill_n(
        std::back_inserter(tensor_parameter_index),
        stack[idx].toListRef().size(),
        idx);
    } else if (stack[idx].isOptionalTensorList()) {
      // optional tensor list: std::vector<std::optional<at::Tensor>>
      for (const auto& item : stack[idx].toListRef()) {
        if (item.toOptional<at::Tensor>().has_value()) {
          tensor_parameter_index.push_back(idx);
        }
      }
    } else if (
        *arguments[idx].real_type() ==
        *c10::getTypePtr<c10::optional<at::Tensor>>()) {
      // optional tensor
      if (stack[idx].toOptional<at::Tensor>().has_value()) {
        tensor_parameter_index.push_back(idx);
      }
    }
  }

  return tensor_parameter_index;
}

} // namespace

AOTIPythonKernelHolder::AOTIPythonKernelHolder(
    py::object func,
    c10::DispatchKey dispatch_key,
    c10::string_view ns,
    c10::string_view op_name,
    c10::string_view op_overload_name)
    : python_kernel_holder_(func, dispatch_key),
      dispatch_key_(dispatch_key),
      ns_(std::string(ns)),
      op_name_(std::string(op_name)),
      op_overload_name_(std::string(op_overload_name)),
      has_fall_back_(func.ptr() != Py_None),
      device_(c10::Device(c10::dispatchKeyToDeviceType(dispatch_key_), 0)),
      pyinterpreter_(getPyInterpreter()) {
  auto pos = op_name_.find("::");
  if (pos != std::string::npos) {
    // Remove the namespace from the op_name as ns is already set
    op_name_ = op_name_.substr(pos + strlen("::"));
  }

  // Initialize the AOTI kernel cache
  init_aoti_kernel_cache();
}

void AOTIPythonKernelHolder::operator()(
    const c10::OperatorHandle& op,
    c10::DispatchKeySet keyset,
    torch::jit::Stack* stack) {
  AOTIKernelState kernel_state;
  if (cache_lookup(op, keyset, stack, kernel_state)) {
    cache_hit(kernel_state, op, keyset, stack);
  } else {
    cache_miss(op, keyset, stack);
  }
}

bool AOTIPythonKernelHolder::cache_lookup(
    const c10::OperatorHandle& op,
    const c10::DispatchKeySet& keyset,
    const torch::jit::Stack* stack,
    AOTIKernelState& kernel_state) {
  auto return_arguments = op.schema().returns();
  // Only support single return value now and will extend to multiple return
  if (return_arguments.size() != 1) {
    return false;
  }

  auto arg = return_arguments[0];
  // Only support return single tensor.
  // TODO: Extend scope to support tensor vector
  if (!arg.type()->isSubtypeOf(*c10::TensorType::get())) {
    return false;
  }

  std::vector<at::Tensor> inputs;
  auto res = unpack_tensors(
      op.schema().arguments(), *stack, device_, inputs, true /*with scalar*/);
  if (!res || inputs.empty()) {
    return false;
  }

  auto tensor_parameter_index =
      get_tensor_paramter_index(op.schema().arguments(), *stack);
  TORCH_INTERNAL_ASSERT(tensor_parameter_index.size() == inputs.size());
  auto inputs_meta_info = get_inputs_meta_info(
      inputs, op.schema().arguments(), tensor_parameter_index);
  auto aoti_kernel_state = aoti_kernel_cache_.find(inputs_meta_info);
  if (aoti_kernel_state == aoti_kernel_cache_.end()) {
    return false;
  }

  if (aoti_kernel_state->second.tensor_checks_.size() != inputs.size()) {
    return false;
  }

  LocalState local_state;
  local_state.overrideDispatchKeySet(c10::DispatchKeySet(dispatch_key_));

  for (size_t i = 0; i < inputs.size(); ++i) {
    bool pass = aoti_kernel_state->second.tensor_checks_[i].check(
        local_state, inputs[i]);
    if (!pass) {
      return false;
    }
  }

  kernel_state = aoti_kernel_state->second;
  return true;
}

void AOTIPythonKernelHolder::cache_hit(
    const AOTIKernelState& kernel_state,
    const c10::OperatorHandle& op,
    const c10::DispatchKeySet& keyset,
    torch::jit::Stack* stack) {
  std::vector<at::Tensor> inputs;
  unpack_tensors(op.schema().arguments(), *stack, device_, inputs);
  torch::jit::drop(*stack, op.schema().arguments().size());

  auto outputs = kernel_state.kernel_runner_->run(inputs);
  for (auto& output : outputs) {
    stack->push_back(output);
  }
}

AOTIKernelMetaInfo AOTIPythonKernelHolder::get_inputs_meta_info(
    const std::vector<at::Tensor>& inputs,
    const std::vector<c10::Argument>& inputs_argument,
    const std::vector<size_t>& inputs_argument_index) {
  AOTIKernelMetaInfo inputs_meta_info;
  for (size_t idx = 0; idx < inputs.size(); ++idx) {
    auto input = inputs[idx];
    auto input_info = inputs_argument[inputs_argument_index[idx]];

    auto device = input.device();
    if (device.is_cpu()) {
      // If the device is CPU, set the device index to -1.
      device = c10::Device(device.type(), -1);
    }

    c10::Scalar scalar_value((double)1.0);
    auto tensor_type = input.scalar_type();

    bool is_scalar = input_info.type()->isSubtypeOf(*c10::NumberType::get());
    if (is_scalar) {
      if (c10::isFloatingType(input.scalar_type())) {
        auto scalar_numeric_value = input.item().toDouble();
        tensor_type = c10::ScalarType::Double;
        scalar_value = c10::Scalar(scalar_numeric_value);
      } else if (c10::isIntegralType(input.scalar_type(), false)) {
        auto scalar_numeric_value = input.item().toUInt64();
        tensor_type = c10::ScalarType::UInt64;
        scalar_value = c10::Scalar(scalar_numeric_value);
      } else if (input.scalar_type() == c10::ScalarType::Bool) {
        auto scalar_numeric_value = input.item().toBool();
        tensor_type = c10::ScalarType::Bool;
        scalar_value = c10::Scalar(scalar_numeric_value);
      } else {
        TORCH_CHECK(
            false,
            "Unsupported scalar tensor type: ",
            c10::toString(input.scalar_type()));
      }
    }

    inputs_meta_info.emplace_back(
        false,
        tensor_type,
        c10::IValue(scalar_value),
        device,
        input.sym_sizes().vec(),
        input.sym_strides().vec());
  }
  return inputs_meta_info;
}

void AOTIPythonKernelHolder::init_aoti_kernel_cache() {
  if (device_.type() == c10::DeviceType::COMPILE_TIME_MAX_DEVICE_TYPES) {
    return;
  }

  py::gil_scoped_acquire gil;

  py::handle load_aoti_eager_cache_function =
      py::module::import("torch._inductor.utils").attr("load_aoti_eager_cache");
  if (load_aoti_eager_cache_function.ptr() == nullptr) {
    return;
  }

  auto result = py::reinterpret_steal<py::object>(PyObject_CallFunctionObjArgs(
      load_aoti_eager_cache_function.ptr(),
      py::str(ns_).ptr(),
      py::str(op_name_).ptr(),
      py::str(op_overload_name_).ptr(),
      py::str(c10::DeviceTypeName(device_.type())).ptr(),
      nullptr));
  if (result.ptr() == nullptr || result.ptr() == Py_None) {
    return;
  }

  auto kernel_info_list = result.cast<py::list>();
  for (auto kernel_info : kernel_info_list) {
    auto item_dict = kernel_info.cast<py::dict>();

    // Access the kernel_path field
    auto kernel_path = item_dict["kernel_path"].cast<std::string>();

    // Access the meta_info list
    auto meta_info_list = item_dict["meta_info"].cast<py::list>();

    std::vector<TensorCheck> tensor_checks;
    std::vector<TensorMetaInfo> tensor_meta_info_list;

    LocalState state;
    // Loop over the meta_info list
    for (auto meta_info : meta_info_list) {
      // Convert the handle to a dict
      auto meta_info_dict = meta_info.cast<py::dict>();

      // Access the fields of each meta_info dict
      auto is_dynamic = meta_info_dict["is_dynamic"].cast<bool>();
      auto device_type = meta_info_dict["device_type"].cast<std::string>();
      auto device_index = meta_info_dict["device_index"].cast<int8_t>();
      auto dtype = meta_info_dict["dtype"].cast<std::string>();
      auto sizes = meta_info_dict["sizes"].cast<std::vector<int64_t>>();
      auto strides = meta_info_dict["strides"].cast<std::vector<int64_t>>();
      bool is_scalar = meta_info_dict.contains("scalar_value");

      std::vector<c10::SymInt> sym_sizes;
      std::vector<c10::SymInt> sym_strides;
      std::vector<std::optional<c10::SymInt>> sym_optional_sizes;
      std::vector<std::optional<c10::SymInt>> sym_optional_strides;
      for (int64_t size : sizes) {
        sym_sizes.push_back(c10::SymInt(size));
        sym_optional_sizes.push_back(std::optional<c10::SymInt>(size));
      }
      for (int64_t stride : strides) {
        sym_strides.push_back(c10::SymInt(stride));
        sym_optional_strides.push_back(std::optional<c10::SymInt>(stride));
      }

      auto tensor_dtype = parse_dtype(dtype);
      // If an input parameter is a scalar, its detailed value is cached.
      // This is done to ensure correctness during subsequent checks.
      c10::Scalar scalar_value((double)1.0);
      if (is_scalar) {
        if (c10::isFloatingType(tensor_dtype)) {
          auto scalar_numeric_value =
              meta_info_dict["scalar_value"].cast<double>();
          tensor_dtype = c10::ScalarType::Double;
          scalar_value = c10::Scalar(scalar_numeric_value);
        } else if (c10::isIntegralType(tensor_dtype, false)) {
          auto scalar_numeric_value =
              meta_info_dict["scalar_value"].cast<int64_t>();
          tensor_dtype = c10::ScalarType::UInt64;
          scalar_value = c10::Scalar(scalar_numeric_value);
        } else if (tensor_dtype == c10::ScalarType::Bool) {
          auto scalar_numeric_value =
              meta_info_dict["scalar_value"].cast<bool>();
          tensor_dtype = c10::ScalarType::Bool;
          scalar_value = c10::Scalar(scalar_numeric_value);
        } else {
          TORCH_CHECK(
              false,
              "Unsupported scalar tensor type: ",
              c10::toString(tensor_dtype));
        }
      }

      tensor_meta_info_list.emplace_back(
          is_dynamic,
          tensor_dtype,
          c10::IValue(scalar_value),
          c10::Device(c10::Device(device_type).type(), device_index),
          sym_sizes,
          sym_strides);
      tensor_checks.emplace_back(
          state,
          nullptr,
          uint64_t(c10::DispatchKeySet(dispatch_key_).raw_repr()),
          tensor_dtype,
          c10::DeviceIndex(device_index),
          sym_optional_sizes,
          sym_optional_strides);
    }

    AOTIKernelState aoti_kernel_state;
    aoti_kernel_state.kernel_runner_ = load_aoti_model_runner(kernel_path);
    aoti_kernel_state.tensor_checks_ = tensor_checks;
    aoti_kernel_cache_[tensor_meta_info_list] = aoti_kernel_state;
  }
}

std::shared_ptr<AOTIModelContainerRunner> AOTIPythonKernelHolder::
    load_aoti_model_runner(const std::string& so_path) {
  if (device_.type() == c10::DeviceType::CUDA) {
#ifdef USE_CUDA
    return std::make_shared<AOTIModelContainerRunnerCpu>(so_path);
#else
    return nullptr;
#endif
  } else if (device_.type() == c10::DeviceType::CPU) {
    return std::make_shared<AOTIModelContainerRunnerCpu>(so_path);
  } else {
    TORCH_WARN("Unsupported device type");
    return nullptr;
  }
}

void AOTIPythonKernelHolder::cache_miss(
    const c10::OperatorHandle& op,
    const c10::DispatchKeySet& keyset,
    torch::jit::Stack* stack) {
  auto device_type = c10::dispatchKeyToDeviceType(dispatch_key_);
  TORCH_CHECK(
      (device_type == c10::DeviceType::CPU) ||
          (device_type == c10::DeviceType::CUDA),
      "Unsupported device type");

  auto kernel_lib_path = produce_aoti_kernel_lib(op, keyset, stack);
  if (kernel_lib_path.empty()) {
    TORCH_CHECK(
        has_fall_back_,
        "Failed to produce kernel libarary by using AOTI and no fall back");
    python_kernel_holder_(op, keyset, stack);
    return;
  }

  auto device_index = 0; // TODO: Get device index from other tensors.
  auto device = c10::Device(device_type, device_index);

  std::shared_ptr<AOTIModelContainerRunner> kernel = nullptr;
  // TODO: To enable the plugin mechanism to allow registration for other
  // backends
  if (device_type == c10::DeviceType::CPU) {
    kernel = std::make_shared<AOTIModelContainerRunnerCpu>(kernel_lib_path);
  } else {
#ifdef USE_CUDA
    kernel = std::make_shared<AOTIModelContainerRunnerCuda>(kernel_lib_path);
#else
    TORCH_CHECK(false, "Unsupported CUDA device type");
#endif
  }

  std::vector<at::Tensor> inputs;
  TORCH_INTERNAL_ASSERT(
      unpack_tensors(op.schema().arguments(), *stack, device, inputs),
      "Failed to unpack tensors for the stack to run the AOTI kernel.");
  auto outputs = kernel->run(inputs);
  if (outputs.size() > 0) {
    torch::jit::drop(*stack, op.schema().arguments().size());
    // TODO: Get the output type of this operation and then convert to the
    // output type.
    for (auto& output : outputs) {
      torch::jit::push(*stack, std::move(output));
    }
  }
}

std::string AOTIPythonKernelHolder::produce_aoti_kernel_lib(
    const c10::OperatorHandle& op,
    const c10::DispatchKeySet& keyset,
    const torch::jit::Stack* stack) {
  auto arguments = torch::jit::last(*stack, op.schema().arguments().size());

  const auto& schema = op.schema();
  const auto& qualified_name = op.operator_name().name;
  const auto& overload_name =
      schema.overload_name().empty() ? "default" : schema.overload_name();
  auto pos = qualified_name.find("::");
  TORCH_INTERNAL_ASSERT(pos != std::string::npos, qualified_name);
  std::string ns_str(qualified_name.begin(), qualified_name.begin() + pos);
  std::string func_name(
      qualified_name.begin() + pos + strlen("::"), qualified_name.end());

  std::string kernel_lib_path("");

  py::gil_scoped_acquire gil;
  py::handle op_py_func = op.getPythonOp(pyinterpreter_, [&]() -> PyObject* {
    py::handle torch_api_function = py::module::import("torch")
                                        .attr("ops")
                                        .attr(ns_str.c_str())
                                        .attr(func_name.c_str());
    if (overload_name.empty()) {
      return torch_api_function.attr("default").ptr();
    } else {
      return torch_api_function.attr(overload_name.c_str()).ptr();
    }
  });

  TORCH_INTERNAL_ASSERT(
      op_py_func.ptr() != nullptr && op_py_func.ptr() != Py_None,
      "Failed to get python operation. Operator Name is ",
      op.operator_name().name,
      ", Overload Name is ",
      overload_name);

  py::handle aot_compile_function =
      py::module::import("torch._inductor.utils")
          .attr("aoti_compile_with_persistent_cache");
  TORCH_INTERNAL_ASSERT(
      aot_compile_function.ptr() != nullptr &&
          aot_compile_function.ptr() != Py_None,
      "Failed to import - torch._inductor.utils.aoti_compile_with_persistent_cache");

  // Pass the python operation to the AOT Inductor to generate the kernel
  // library.
  auto args_kwargs = parseIValuesToPyArgsKwargs(op, arguments.vec());
  auto result = py::reinterpret_steal<py::object>(PyObject_CallFunctionObjArgs(
      aot_compile_function.ptr(),
      py::str(ns_str).ptr(),
      py::str(func_name).ptr(),
      py::str(overload_name).ptr(),
      py::str(c10::DeviceTypeName(device_.type())).ptr(),
      py::bool_(false).ptr(),
      op_py_func.ptr(),
      args_kwargs.first.ptr(),
      args_kwargs.second.ptr(),
      nullptr));
  if (result.ptr() != nullptr && result.ptr() != Py_None) {
    kernel_lib_path = py::cast<std::string>(result);
    if (kernel_lib_path.empty()) {
      TORCH_WARN(
          "Kernel library is not generated by AOTI for ",
          c10::DeviceTypeName(device_.type()),
          ". Operator Name is ",
          op.operator_name().name,
          ", Overload Name is ",
          overload_name);
    }
  } else {
    TORCH_WARN(
        "AOTI kernel library is not generated for ",
        c10::DeviceTypeName(device_.type()),
        ". Operator Name is ",
        op.operator_name().name,
        ", Overload Name is ",
        overload_name);
  }

  return kernel_lib_path;
}

} // namespace torch::inductor
#endif
