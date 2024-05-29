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
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <nvml.h>
#include <optional>
#include <string>
#include <vector>

// structure representing throughput measured by B/s
struct throughput {
  unsigned int receive;
  unsigned int transmit;
};

// information of a device
//  W.I.P, fields in this struct are subject to change
struct device_information {
  // stable information, these information is not likely to change in a relative long period
  nvmlDevice_t               handle;     // the handle of device
  unsigned int               id;         // index of the device
  std::string                name;       // name of the device
  std::string                serial;     // board serial number of the device
  nvmlPciInfo_t              pci;        // PCI bus information about the device
  std::optional<std::string> uuid;       // uuid of the device, might unavailable
  nvmlDeviceAttributes_t     attributes; // attributes of the device

  // volatile information, these information may change rapidly

  // time point at which volatile information is sampled
  std::chrono::time_point<std::chrono::system_clock> sample_time;
  throughput                                         pcie_throughput; // throughput of PCIe
  nvmlMemory_t                                       memory;          // memory statistics

  // construct by directly query with the NVML library
  device_information(nvmlDevice_t device);
  // resample volatile information
  void               resample();
  // get computation processes on this device
  [[nodiscard]] auto get_processes() const -> std::vector<nvmlProcessInfo_t>;
};

class NVMLSessionManager {
private:
  NVMLSessionManager();
  ~NVMLSessionManager();

public:
  static auto get_manager() -> NVMLSessionManager &;
  // make a vector of device_information to all accessible devices on the system
  auto        get_device_informations() -> std::vector<device_information>;
};

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
#endif