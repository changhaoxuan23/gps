// hooked_api - Source file implementing hooked CUDA APIs
// Copyright (C) 2023-2024 Haoxuan Chang<changhaoxuan23@mails.ucas.ac.cn>

// This is part of gps.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.

// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

// real implementation of hooked CUDA APIs will be appended to a exact copy of this file
//  all functions defined here will and will only be used by the hooked APIs, therefore they are declared
//   as static, and inline in case it is simple enough and would expect frequent call
//  standard attribute [[maybe_unused]] is used for each declaration only to silent warnings
#ifndef SCANNING
#include <hooked_api_config.hh>

#include <concepts>
#include <cstdlib>
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <memory>
#include <print>
#include <regex>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#ifdef HAVE_PYTHON
#include <Python.h>
#include <format>
#endif // HAVE_PYTHON

struct SinkSpecification {
  int   fd;
  FILE *file;
  struct {
    uint8_t include_backtrace : 1;
    uint8_t supports_color : 1;
  } property;
};

#ifdef HAVE_PYTHON
[[maybe_unused]] static inline auto format_python_unicode(PyObject *unicode) -> std::string {
  Py_ssize_t size   = 0;
  auto       string = PyUnicode_AsUTF8AndSize(unicode, &size);
  if (string == nullptr) {
    return "???";
  }
  return {string, static_cast<size_t>(size)};
}

[[maybe_unused]] static auto collect_python_backtrace() -> std::vector<std::string> {
  std::vector<std::string> traces;
  auto                     gil    = PyGILState_Ensure();
  PyThreadState           *tstate = PyGILState_GetThisThreadState();

  auto frame = PyThreadState_GetFrame(tstate);

  while (frame != nullptr) {
    PyCodeObject *code          = PyFrame_GetCode(frame);
    auto          function_name = format_python_unicode(code->co_name);
    auto          filename      = format_python_unicode(code->co_filename);
    auto          line_number   = PyFrame_GetLineNumber(frame);

    traces.emplace_back(std::format("{} in {}:{}", function_name, filename, line_number));

    frame = PyFrame_GetBack(frame);
  }
  PyGILState_Release(gil);
  return traces;
}
#endif // HAVE_PYTHON

[[maybe_unused]] static auto collect_physical_backtrace() -> std::vector<std::string> {
  std::vector<void *> return_addresses(32);
  while (true) {
    auto size = backtrace(return_addresses.data(), static_cast<int>(return_addresses.size()));
    if (size >= static_cast<int>(return_addresses.size())) {
      return_addresses.resize(return_addresses.size() * 2);
      continue;
    }
    return_addresses.resize(size);
    break;
  }
  auto trace = backtrace_symbols(return_addresses.data(), static_cast<int>(return_addresses.size()));
  std::vector<std::string> result{trace, trace + return_addresses.size()};
  free(trace);
  return result;
}

// print a stacktrace stored in a vector of strings like
template <std::convertible_to<std::string_view> T1, std::convertible_to<std::string_view> T2>
[[maybe_unused]] static void print_backtrace(T1 name, const std::vector<T2> &trace, SinkSpecification sink) {
  constexpr const char *StartLine = "vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv";
  constexpr const char *EndLine   = "^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^";
  std::println(sink.file, "{} {} {}", StartLine, name, StartLine);
  for (size_t i = 0; i < trace.size(); i++) {
    std::println(
      sink.file,
      "  {}#{}{} {}",
      sink.property.supports_color ? "\x1b[1;36m" : "",
      i,
      sink.property.supports_color ? "\x1b[0m" : "",
      trace[i]
    );
  }
  std::println(sink.file, "{} {} {}", EndLine, name, EndLine);
}

[[maybe_unused]] static auto
collect_backtraces() -> std::unordered_map<std::string_view, std::vector<std::string>> {
  std::unordered_map<std::string_view, std::vector<std::string>> result;

#ifdef HAVE_PYTHON
  if (Py_IsInitialized()) {
    result.emplace("python", collect_python_backtrace());
  }
#endif // HAVE_PYTHON

  result.emplace("physical", collect_physical_backtrace());
  return result;
}

[[maybe_unused]] static inline auto get_cuda_function(const char *name) -> void * {
  return dlsym(RTLD_NEXT, name);
}

static auto get_configuration() -> const GPSHookedAPIConfiguration & {
  static auto configuration = get_hooked_api_configuration();
  if (!configuration) [[unlikely]] {
    configuration          = std::make_unique<GPSHookedAPIConfiguration>();
    auto &sink             = configuration->outputs.emplace_back();
    sink.include_backtrace = true;
    sink.output_fd         = STDERR_FILENO;
  }
  return *configuration;
}
static auto get_sinks() -> const std::vector<SinkSpecification> & {
  static std::vector<SinkSpecification> result;
  if (result.empty()) {
    const auto &configuration = get_configuration();
    for (const auto &sink_ : configuration.outputs) {
      auto &sink                      = result.emplace_back();
      sink.fd                         = sink_.output_fd;
      sink.property.include_backtrace = sink_.include_backtrace;
      sink.file                       = fdopen(sink.fd, "w");
      sink.property.supports_color    = isatty(sink.fd);
    }
  }
  return result;
}
[[maybe_unused]] static void
handle_output(std::vector<bool> mask, bool request_trace, const std::string &data, bool before_return) {
  static std::vector<SinkSpecification> sinks = get_sinks();
  auto                                  traces =
    request_trace ? collect_backtraces() : std::unordered_map<std::string_view, std::vector<std::string>>();
  for (size_t i = 0; i < sinks.size(); i++) {
    if (!mask[i]) {
      continue;
    }
    if (before_return && sinks[i].property.include_backtrace) {
      std::println(sinks[i].file, "");
      for (const auto &[name, trace] : traces) {
        print_backtrace(name, trace, sinks[i]);
      }
      std::println(sinks[i].file, "This was stack trace of the following CUDA API call:");
    }
    if (sinks[i].property.supports_color) {
      std::print(sinks[i].file, "\x1b[1;36m{}\x1b[0m", data);
    } else {
      std::print(sinks[i].file, "{}", data);
    }
    fflush(sinks[i].file);
  }
}
// get mask about current API:
//  .first -> the mask
//  .second -> does any active sink requests backtrace
[[maybe_unused]] static auto get_mask(const char *name) -> std::pair<std::vector<bool>, bool> {
  static const auto             &sinks = get_sinks();
  static std::vector<std::regex> filters;
  if (filters.empty()) [[unlikely]] {
    const auto &configuration = get_configuration();
    for (const auto &sink : configuration.outputs) {
      if (sink.filter.empty()) {
        filters.emplace_back(".+");
      } else {
        filters.emplace_back(sink.filter);
      }
    }
  }

  std::vector<bool> mask;
  bool              request_trace = false;
  mask.reserve(filters.size());
  for (size_t i = 0; i < filters.size(); i++) {
    auto result = std::regex_match(name, filters[i]);
    mask.emplace_back(result);
    if (result && sinks[i].property.include_backtrace) {
      request_trace = true;
    }
  }
  return {mask, request_trace};
}
#endif // SCANNING

// ====== begin of functions handled manually ====== //

auto cudaMalloc(void **devPtr, size_t size) -> cudaError_t {
  static const auto real_cudaMalloc =
    reinterpret_cast<decltype(&cudaMalloc)>(get_cuda_function("cudaMalloc"));
  static const auto [mask, require_trace] = get_mask("cudaMalloc");

  handle_output(mask, require_trace, "cudaMalloc(", true);
  auto trace_internal_result = real_cudaMalloc(devPtr, size);
  auto result_pointer =
    devPtr == nullptr ? "<nullptr>" : std::format("[0x{:016x}]", reinterpret_cast<uintmax_t>(*devPtr));
  handle_output(
    mask,
    require_trace,
    std::format(
      "devPtr={}, size={}) -> {} ({}: {})\n",
      result_pointer,
      size,
      static_cast<int>(trace_internal_result),
      cudaGetErrorName(trace_internal_result),
      cudaGetErrorString(trace_internal_result)
    ),
    false
  );
  return trace_internal_result;
}

auto cudaMalloc3D(struct cudaPitchedPtr *pitchedDevPtr, struct cudaExtent extent) -> cudaError_t {
  static const auto real_cudaMalloc3D =
    reinterpret_cast<decltype(&cudaMalloc3D)>(get_cuda_function("cudaMalloc3D"));
  static const auto [mask, require_trace] = get_mask("cudaMalloc3D");

  handle_output(mask, require_trace, "cudaMalloc3D(", true);
  auto trace_internal_result = real_cudaMalloc3D(pitchedDevPtr, extent);
  handle_output(
    mask,
    require_trace,
    std::format(
      "pitchedDevPtr=[{{pitch={}, ptr=0x{:016x}, xsize={}, ysize={}}}], extent={{depth={}, height={}, "
      "width={}}}) -> {} ({}: {})\n",
      pitchedDevPtr->pitch,
      reinterpret_cast<uintmax_t>(pitchedDevPtr->ptr),
      pitchedDevPtr->xsize,
      pitchedDevPtr->ysize,
      extent.depth,
      extent.height,
      extent.width,
      static_cast<int>(trace_internal_result),
      cudaGetErrorName(trace_internal_result),
      cudaGetErrorString(trace_internal_result)
    ),
    false
  );
  return trace_internal_result;
}

auto cudaMallocManaged(void **devPtr, size_t size, unsigned int flags) -> cudaError_t {
  static const auto real_cudaMallocManaged =
    reinterpret_cast<decltype(&cudaMallocManaged)>(get_cuda_function("cudaMallocManaged"));
  static const auto [mask, require_trace] = get_mask("cudaMallocManaged");

  handle_output(mask, require_trace, "cudaMallocManaged(", true);
  auto trace_internal_result = real_cudaMallocManaged(devPtr, size, flags);
  handle_output(
    mask,
    require_trace,
    std::format(
      "devPtr=[0x{:016x}], size={}, flags={}) -> {} ({}: {})\n",
      reinterpret_cast<uintmax_t>(*devPtr),
      size,
      flags == cudaMemAttachGlobal ? "cudaMemAttachGlobal" : "cudaMemAttachHost",
      static_cast<int>(trace_internal_result),
      cudaGetErrorName(trace_internal_result),
      cudaGetErrorString(trace_internal_result)
    ),
    false
  );
  return trace_internal_result;
}

auto cudaMallocPitch(void **devPtr, size_t *pitch, size_t width, size_t height) -> cudaError_t {
  static const auto real_cudaMallocPitch =
    reinterpret_cast<decltype(&cudaMallocPitch)>(get_cuda_function("cudaMallocPitch"));
  static const auto [mask, require_trace] = get_mask("cudaMallocPitch");

  handle_output(mask, require_trace, "cudaMallocPitch(", true);
  auto trace_internal_result = real_cudaMallocPitch(devPtr, pitch, width, height);
  handle_output(
    mask,
    require_trace,
    std::format(
      "devPtr=[0x{:016x}], pitch=[{}], width={}, height={}) -> {} ({}: {})\n",
      reinterpret_cast<uintmax_t>(*devPtr),
      *pitch,
      width,
      height,
      static_cast<int>(trace_internal_result),
      cudaGetErrorName(trace_internal_result),
      cudaGetErrorString(trace_internal_result)
    ),
    false
  );
  return trace_internal_result;
}

auto cudaMallocFromPoolAsync(void **ptr, size_t size, cudaMemPool_t memPool, cudaStream_t stream)
  -> cudaError_t {
  static const auto real_cudaMallocFromPoolAsync =
    reinterpret_cast<decltype(&cudaMallocFromPoolAsync)>(get_cuda_function("cudaMallocFromPoolAsync"));
  static const auto [mask, require_trace] = get_mask("cudaMallocFromPoolAsync");

  handle_output(mask, require_trace, "cudaMallocFromPoolAsync(", true);
  auto trace_internal_result = real_cudaMallocFromPoolAsync(ptr, size, memPool, stream);
  handle_output(
    mask,
    require_trace,
    std::format(
      "ptr=[0x{:016x}], size={}, memPool=0x{:016x}, stream=0x{:016x}) -> {} ({}: {})\n",
      reinterpret_cast<uintmax_t>(*ptr),
      size,
      reinterpret_cast<uintmax_t>(memPool),
      reinterpret_cast<uintmax_t>(stream),
      static_cast<int>(trace_internal_result),
      cudaGetErrorName(trace_internal_result),
      cudaGetErrorString(trace_internal_result)
    ),
    false
  );
  return trace_internal_result;
}

auto cudaMallocAsync(void **devPtr, size_t size, cudaStream_t hStream) -> cudaError_t {
  static const auto real_cudaMallocAsync =
    reinterpret_cast<decltype(&cudaMallocAsync)>(get_cuda_function("cudaMallocAsync"));
  static const auto [mask, require_trace] = get_mask("cudaMallocAsync");

  handle_output(mask, require_trace, "cudaMallocAsync(", true);
  auto trace_internal_result = real_cudaMallocAsync(devPtr, size, hStream);
  handle_output(
    mask,
    require_trace,
    std::format(
      "devPtr=[0x{:016x}], size={}, hStream=0x{:016x}) -> {} ({}: {})\n",
      reinterpret_cast<uintmax_t>(*devPtr),
      size,
      reinterpret_cast<uintmax_t>(hStream),
      static_cast<int>(trace_internal_result),
      cudaGetErrorName(trace_internal_result),
      cudaGetErrorString(trace_internal_result)
    ),
    false
  );
  return trace_internal_result;
}

// ======= end of functions handled manually ======= //

//////////////////////////////////////////////////////////////
// ====== begin of functions generated automatically ====== //
//////////////////////////////////////////////////////////////