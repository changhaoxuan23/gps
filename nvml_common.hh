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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <nvml.h>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
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

// structure representing throughput measured by B/s
struct throughput {
  unsigned long long receive;
  unsigned long long transmit;
};

// information of a device
//  W.I.P, fields in this struct are subject to change
struct device_information {
  // stable information, these information is not likely to change in a relative long period
  nvmlDevice_t handle;               // the handle of device
  unsigned int id;                   // index of the device
  std::string name;                  // name of the device
  std::string serial;                // board serial number of the device
  nvmlPciInfo_t pci;                 // PCI bus information about the device
  std::optional<std::string> uuid;   // uuid of the device, might unavailable
  nvmlDeviceAttributes_t attributes; // attributes of the device

  // volatile information, these information may change rapidly
  // time point at which volatile information is sampled
  std::chrono::time_point<std::chrono::system_clock> sample_time;
  throughput pcie_throughput; // throughput of PCIe
  nvmlMemory_t memory;        // memory statistics

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

// get information of all computation processes on given device specified by a device handle
//  return a std::pair, in which the first element has type nvmlReturn_t that is the return value from
//   underlying API call, while the second element has type std::vector<nvmlProcessInfo_t>> that is an array
//   of structures defined by NVML, one for each process running on the given device, contains valid content
//   if and only if the first element in the pair indicates a successful call
auto get_processes_on_device(nvmlDevice_t device) -> std::pair<nvmlReturn_t, std::vector<nvmlProcessInfo_t>>;

// get human-readable representation of duration given in unit of seconds
//  due to ambiguity, no unit representing more seconds than day will be involved in the result, that is, only
//  the following units may be employed:
//    day    ---- 86400s
//    hour   ----  3600s
//    minute ----    60s
//    second ----     1s
auto get_readable_duration(unsigned long long seconds) -> std::string;

// get human-readable representation of size given in unit of byte
auto get_readable_size(unsigned long long value) -> std::string;

// get mapping from size suffixes to multiplier
//  for simplicity, suffixes are converted into lowercase
auto get_size_suffix_map() -> const std::unordered_map<std::string, unsigned long long> &;

// get mapping from duration suffixes to multiplier
//  for simplicity, suffixes are converted into lowercase
auto get_duration_suffix_map() -> const std::unordered_map<std::string, unsigned long long> &;
#endif