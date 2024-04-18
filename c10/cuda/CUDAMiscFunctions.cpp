#include <cstdlib>
#include <c10/cuda/CUDAMiscFunctions.h>
#include <c10/util/env.h>

namespace c10::cuda {

const char* get_cuda_check_suffix() noexcept {
  static auto device_blocking_flag =
      c10::utils::get_env("CUDA_LAUNCH_BLOCKING");
  static bool blocking_enabled =
      (device_blocking_flag && std::stoi(device_blocking_flag.value()));
  if (blocking_enabled) {
    return "";
  } else {
    return "\nCUDA kernel errors might be asynchronously reported at some"
           " other API call, so the stacktrace below might be incorrect."
           "\nFor debugging consider passing CUDA_LAUNCH_BLOCKING=1.";
  }
}
std::mutex* getFreeMutex() {
  static std::mutex cuda_free_mutex;
  return &cuda_free_mutex;
}

} // namespace c10::cuda
