// nvml_common.hh - commonly used utility functions interacting with NVML
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

#ifndef NVML_COMMON_HH_
#define NVML_COMMON_HH_
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <nvml.h>
#include <string>
#include <utility>
#include <vector>
#define panic_on_failure(nvml_call, ...)                                                                     \
  do {                                                                                                       \
    nvmlReturn_t return_value = nvml_call(__VA_ARGS__);                                                      \
    if (return_value != NVML_SUCCESS) {                                                                      \
      fprintf(stderr, "error on " #nvml_call ": %s\n", nvmlErrorString(return_value));                       \
      exit(EXIT_FAILURE);                                                                                    \
    }                                                                                                        \
  } while (false)
struct device_information {
  unsigned int id;     // index of the device
  std::string name;    // name of the device
  nvmlMemory_t memory; // memory statistics

  // construct by directly query with the NVML library
  device_information(nvmlDevice_t device) {
    std::array<char, NVML_DEVICE_NAME_V2_BUFFER_SIZE> buffer;
    nvmlReturn_t return_value;
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
    return_value = nvmlDeviceGetMemoryInfo(device, std::addressof(this->memory));
    if (return_value != NVML_SUCCESS) {
      fprintf(
          stderr, "failed to get device memory statistics for %u: %s\n", this->id,
          nvmlErrorString(return_value)
      );
      this->memory.free = NVML_VALUE_NOT_AVAILABLE;
      this->memory.used = NVML_VALUE_NOT_AVAILABLE;
      this->memory.total = NVML_VALUE_NOT_AVAILABLE;
    }
  }
};
static auto get_processes_on_device(nvmlDevice_t device)
    -> std::pair<nvmlReturn_t, std::vector<nvmlProcessInfo_t>> {
  unsigned int process_count = 0;
  nvmlProcessInfo_t *information = nullptr;
  nvmlReturn_t return_value;
  while (true) {
    // since the number of process may change, we need to loop and keep increasing the size of buffer until
    //  we can finally during some call to the API have sufficient space for all processes running
    return_value = nvmlDeviceGetComputeRunningProcesses(device, &process_count, information);
    if (return_value != NVML_ERROR_INSUFFICIENT_SIZE) {
      if (return_value != NVML_SUCCESS) {
        free(information);
        information = nullptr;
      }
      break;
    } else {
      void *new_buffer = realloc(information, sizeof(nvmlProcessInfo_t) * process_count);
      if (new_buffer == nullptr) {
        free(information);
        information = nullptr;
        return_value = NVML_ERROR_INSUFFICIENT_SIZE;
        break;
      }
      information = static_cast<nvmlProcessInfo_t *>(new_buffer);
    }
  }
  if (information == nullptr) {
    return std::make_pair(return_value, std::vector<nvmlProcessInfo_t>());
  }
  std::vector<nvmlProcessInfo_t> result(information, information + process_count);
  return std::make_pair(NVML_SUCCESS, result);
}
static inline auto get_readable_duration(unsigned long long seconds) -> std::string {
  const static std::array<std::string, 4> suffixes = {"day(s)", "hour(s)", "minute(s)", "second(s)"};
  const static std::array<unsigned int, 4> ratios = {0, 24, 60, 60};
  std::array<unsigned long long, 4> values;
  values.back() = seconds;
  for (size_t i = values.size() - 1; i != 0; i--) {
    values.at(i - 1) = values.at(i) / ratios.at(i);
    values.at(i) %= ratios.at(i);
  }
  bool start = false;
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
static inline auto get_readable_size(unsigned long long value) -> std::string {
  std::array<std::string, 6> suffixes = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
  long double temporary = value;
  unsigned int selection = 0;
  while (selection < suffixes.size() - 1) {
    if (temporary / 1024 > 1000) {
      temporary /= 1024;
      selection += 1;
    } else {
      break;
    }
  }
  value = static_cast<unsigned long long>(std::round(temporary));
  return std::to_string(value) + suffixes[selection];
}
#endif