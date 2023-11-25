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

#include <nvml_common.hh>
auto get_processes_on_device(nvmlDevice_t device) -> std::pair<nvmlReturn_t, std::vector<nvmlProcessInfo_t>> {
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

auto get_readable_duration(unsigned long long seconds) -> std::string {
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

auto get_readable_size(unsigned long long value) -> std::string {
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

auto get_size_suffix_map() -> const std::unordered_map<std::string, unsigned long long> & {
  static bool initialize = true;
  static std::unordered_map<std::string, unsigned long long> map;
  if (initialize) {
    map.insert({"kib", 1024ull});
    map.insert({"kb", 1024ull});
    map.insert({"k", 1024ull});

    map.insert({"mib", 1024ull * 1024});
    map.insert({"mb", 1024ull * 1024});
    map.insert({"m", 1024ull * 1024});

    map.insert({"gib", 1024ull * 1024 * 1024});
    map.insert({"gb", 1024ull * 1024 * 1024});
    map.insert({"g", 1024ull * 1024 * 1024});

    map.insert({"tib", 1024ull * 1024 * 1024 * 1024});
    map.insert({"tb", 1024ull * 1024 * 1024 * 1024});
    map.insert({"t", 1024ull * 1024 * 1024 * 1024});

    map.insert({"pib", 1024ull * 1024 * 1024 * 1024 * 1024});
    map.insert({"pb", 1024ull * 1024 * 1024 * 1024 * 1024});
    map.insert({"p", 1024ull * 1024 * 1024 * 1024 * 1024});
  }
  return map;
}
auto get_duration_suffix_map() -> const std::unordered_map<std::string, unsigned long long> & {
  static bool initialize = true;
  static std::unordered_map<std::string, unsigned long long> map;
  if (initialize) {
    map.insert({"m", 60ull});
    map.insert({"minute", 60ull});
    map.insert({"minutes", 60ull});

    map.insert({"h", 60ull * 60});
    map.insert({"hour", 60ull * 60});
    map.insert({"hours", 60ull * 60});

    map.insert({"d", 60ull * 60 * 24});
    map.insert({"day", 60ull * 60 * 24});
    map.insert({"days", 60ull * 60 * 24});
  }
  return map;
}