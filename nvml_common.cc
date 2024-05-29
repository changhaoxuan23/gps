// nvml_common.cc - commonly used utility functions interacting with NVML
// Copyright (C) 2023 Haoxuan Chang<changhaoxuan23@mails.ucas.ac.cn>

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

#include "nvml_common.hh"
#include <array>
#include <chrono>
#include <memory>
#include <nvml.h>
#include <print>
// assert successful API call to NVML
//  on failure, this macro shows the name of the failed call, shows a string explaining the error provided by
//  NVML, and terminate the process with exit(EXIT_FAILURE)
// parameters: the API function followed by all arguments to be passed to such API
#define panic_on_failure(nvml_call, ...)                                                                     \
  do {                                                                                                       \
    nvmlReturn_t return_value = nvml_call(__VA_ARGS__);                                                      \
    if (return_value != NVML_SUCCESS) {                                                                      \
      fprintf(stderr, "error on " #nvml_call ": %s\n", nvmlErrorString(return_value));                       \
      exit(EXIT_FAILURE);                                                                                    \
    }                                                                                                        \
  } while (false)

NVMLSessionManager::NVMLSessionManager() { panic_on_failure(nvmlInit); }
NVMLSessionManager::~NVMLSessionManager() { nvmlShutdown(); }
auto NVMLSessionManager::get_manager() -> NVMLSessionManager & {
  static NVMLSessionManager manager;
  return manager;
}
auto NVMLSessionManager::get_device_informations() -> std::vector<device_information> {
  unsigned int device_count = 0;
  panic_on_failure(nvmlDeviceGetCount, &device_count);
  std::vector<device_information> devices;
  devices.reserve(device_count);
  for (unsigned int i = 0; i < device_count; i++) {
    nvmlDevice_t device       = nullptr;
    nvmlReturn_t return_value = nvmlDeviceGetHandleByIndex(i, &device);
    if (return_value != NVML_SUCCESS) {
      fprintf(stderr, "failed to open device %u: %s, skipping.\n", i, nvmlErrorString(return_value));
      continue;
    }
    devices.emplace_back(device);
  }
  return devices;
}

device_information::device_information(nvmlDevice_t device) : handle(device) {
  std::array<char, NVML_DEVICE_NAME_V2_BUFFER_SIZE> buffer;
  nvmlReturn_t                                      return_value;
  return_value = nvmlDeviceGetIndex(device, std::addressof(this->id));
  if (return_value != NVML_SUCCESS) {
    fprintf(stderr, "failed to get device id: %s\n", nvmlErrorString(return_value));
    this->id = -1;
  }
  return_value = nvmlDeviceGetName(device, buffer.data(), buffer.size());
  if (return_value != NVML_SUCCESS) {
    fprintf(stderr, "failed to get device name for %u: %s\n", this->id, nvmlErrorString(return_value));
    buffer.at(0) = '\0';
  }
  this->name = buffer.data();
  this->resample();
}

void device_information::resample() {
  this->sample_time = std::chrono::system_clock::now();
  nvmlReturn_t return_value;
  return_value =
    nvmlDeviceGetPcieThroughput(this->handle, NVML_PCIE_UTIL_TX_BYTES, &this->pcie_throughput.transmit);
  if (return_value != NVML_SUCCESS) {
    std::println(
      stderr, "failed to get device throughput for {}: {}", this->id, nvmlErrorString(return_value)
    );
    this->pcie_throughput.transmit = NVML_VALUE_NOT_AVAILABLE;
  }
  return_value =
    nvmlDeviceGetPcieThroughput(this->handle, NVML_PCIE_UTIL_RX_BYTES, &this->pcie_throughput.receive);
  if (return_value != NVML_SUCCESS) {
    std::println(
      stderr, "failed to get device throughput for {}: {}", this->id, nvmlErrorString(return_value)
    );
    this->pcie_throughput.receive = NVML_VALUE_NOT_AVAILABLE;
  }
  return_value = nvmlDeviceGetMemoryInfo(this->handle, std::addressof(this->memory));
  if (return_value != NVML_SUCCESS) {
    std::println(
      stderr, "failed to get device memory statistics for {}: {}", this->id, nvmlErrorString(return_value)
    );
    this->memory.free  = NVML_VALUE_NOT_AVAILABLE;
    this->memory.used  = NVML_VALUE_NOT_AVAILABLE;
    this->memory.total = NVML_VALUE_NOT_AVAILABLE;
  }
}

auto device_information::get_processes() const -> std::vector<nvmlProcessInfo_t> {
  unsigned int                         process_count = 0;
  std::unique_ptr<nvmlProcessInfo_t[]> information;
  nvmlReturn_t                         return_value;
  while (true) {
    // since the number of process may change, we need to loop and keep
    // increasing the size of buffer until
    //  we can finally during some call to the API have sufficient space for all
    //  processes running
    return_value = nvmlDeviceGetComputeRunningProcesses(this->handle, &process_count, information.get());
    if (return_value != NVML_ERROR_INSUFFICIENT_SIZE) {
      if (return_value != NVML_SUCCESS) {
        information.reset();
      }
      break;
    } else {
      information = std::make_unique<nvmlProcessInfo_t[]>(process_count);
    }
  }
  if (information.get() == nullptr) {
    return {};
  }
  return {information.get(), information.get() + process_count};
}

auto get_readable_duration(unsigned long long seconds) -> std::string {
  const static std::array<std::string, 4>  suffixes = {"day(s)", "hour(s)", "minute(s)", "second(s)"};
  const static std::array<unsigned int, 4> ratios   = {0, 24, 60, 60};
  std::array<unsigned long long, 4>        values;
  values.back() = seconds;
  for (size_t i = values.size() - 1; i != 0; i--) {
    values.at(i - 1) = values.at(i) / ratios.at(i);
    values.at(i) %= ratios.at(i);
  }
  bool        start = false;
  std::string result;
  for (size_t i = 0; i < values.size(); i++) {
    if (values.at(i) != 0) {
      start = true;
    }
    if (start) {
      result += std::to_string(values.at(i)) + " " + suffixes.at(i);
      if (i != values.size() - 1) {
        result += ", ";
      }
    }
  }
  if (!start) {
    result = "0 second";
  }
  return result;
}

auto get_readable_size(unsigned long long value) -> std::string {
  std::array<std::string, 6> suffixes  = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
  long double                temporary = value;
  unsigned int               selection = 0;
  while (selection < suffixes.size() - 1) {
    if (temporary / 1024 > 100) {
      temporary /= 1024;
      selection += 1;
    } else {
      break;
    }
  }
  value = static_cast<unsigned long long>(std::round(temporary));
  return std::to_string(value) + suffixes[selection];
}