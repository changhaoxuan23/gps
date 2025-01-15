// gps - list all compute processes running on gpu with detailed information
// Copyright (C) 2023-2025 Haoxuan Chang<changhaoxuan23@mails.ucas.ac.cn>

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

#include <gps-internal/filter.hh>
#include <gps-internal/process_information.hh>
#include <nvml_common.hh>

#include <algorithm>
#include <cerrno>
#include <map>
#include <print>
#include <sys/types.h>
#include <vector>

auto main(int argc, char **argv) -> int {
  std::println("gps in gps build v{}, licensed under AGPLv3 or later", GPS_VERSION);
  std::println("you can goto https://github.com/changhaoxuan23/gps for source code\n");

  // 1. parse commandline and build a filter
  auto filter = GPS::Filter::build_filter(argc, argv);
#ifndef NDEBUG
  filter->print();
#endif

  // 2. prepare a vector of all processes that are running on some GPU device
  //  we do this by:
  //   1. list all GPU devices
  //   2. foreach GPU device, we list all processes running on it
  //   3. foreach processes listed, record it if it is not yet recorded, otherwise update the record

  // 2.1. prepare the process mapping which maps from pid to process information
  std::map<pid_t, GPS::process_information> processes_;
  // 2.2. gather processes
  auto devices = NVMLSessionManager::get_manager().get_device_informations();
  std::ranges::for_each(devices, [&processes_](const device_information &device) {
    std::ranges::for_each(
      device.get_processes(),
      [&processes_, &device](const nvmlProcessInfo_t &nvml_process) {
        // NVML use unsigned int as the type of its pid field, we shall cast this
        auto pid  = static_cast<pid_t>(nvml_process.pid);
        auto iter = processes_.find(pid);
        if (iter == processes_.end()) {
          // this process is not yet recorded
          iter = processes_.emplace(pid, pid).first;
        }
        // anyway, add information about size of GPU memory used by this process
        iter->second.devices.emplace_back(device, nvml_process.usedGpuMemory);
      }
    );
  });
  // 2.3. reshape the map into a vector
  std::vector<GPS::process_information> processes;
  processes.reserve(processes_.size());
  for (auto &[key, value] : processes_) {
    processes.emplace_back(std::move(value));
  }
  processes_.clear();

  // 3. run filters on the prepared vector
  std::ranges::for_each(processes, [&filter](const GPS::process_information &process) {
    static_cast<void>(filter->evaluate(process));
  });
  return 0;
}