// hooked_api_config.cc - implementation of configuring interface to the hooked CUDA APIs
// availability Copyright (C) 2024-2025 Haoxuan Chang<changhaoxuan23@mails.ucas.ac.cn>

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
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <hooked_api_config.hh>
#include <iterator>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>

// environment variable storing the address to the configuration structure
constexpr const char *GPS_HOOKED_API_CONFIGURATION_ENV = "GPS_HOOKED_API_CONFIGURATION";

static auto get_file_path() -> std::string_view {
  const auto raw_path = getenv(GPS_HOOKED_API_CONFIGURATION_ENV);
  if (raw_path == nullptr) {
    return {};
  }
  return raw_path;
}

void setup_api_hook(GPSHookedAPIConfiguration &&config) {
  auto existing_configuration = get_hooked_api_configuration();
  if (existing_configuration) {
    std::ranges::move(existing_configuration->outputs, std::back_insert_iterator(config.outputs));
  }
  std::string path(get_file_path());
  if (path.empty()) {
    // make temporary file to pass the configuration
    path = (std::filesystem::temp_directory_path() / std::format("hooked-api-config-{}.XXXXXX", getpid()))
             .generic_string();
    setenv(GPS_HOOKED_API_CONFIGURATION_ENV, path.c_str(), 1);

    // add hooked API to be preloaded to newly exec'ed process
    const std::filesystem::path preloader(INSTALL_PREFIX "/lib/libhooked_api.so");
    if (!std::filesystem::exists(preloader)) {
      std::println(stderr, "cannot find {}, please check your installation", preloader.string());
      exit(EXIT_FAILURE);
    }
    const auto original_preload = getenv("LD_PRELOAD");
    const auto string_preload   = preloader.string();
    if (original_preload == nullptr) {
      setenv("LD_PRELOAD", string_preload.c_str(), 1);
    } else {
      std::string_view original_preload_string(original_preload);
      std::string      final_preload = string_preload;
      if (!original_preload_string.empty()) {
        final_preload.push_back(':');
        final_preload.append(original_preload_string);
      }
      setenv("LD_PRELOAD", final_preload.c_str(), 1);
    }
  }
  std::ofstream output(path);
  for (const auto &sink : config.outputs) {
    output << sink.output_fd << ' ' << sink.include_backtrace << ' ' << sink.filter << '\n';
  }
}

auto get_hooked_api_configuration() -> std::unique_ptr<GPSHookedAPIConfiguration> {
  auto path = get_file_path();
  if (path.empty()) {
    return nullptr;
  }
  if (access(path.data(), R_OK) == -1) {
    return nullptr;
  }

  auto          result = std::make_unique<GPSHookedAPIConfiguration>();
  std::ifstream input(path.data());
  std::string   line_buffer;
  while (true) {
    line_buffer.clear();
    std::getline(input, line_buffer);
    if (line_buffer.empty()) {
      if (input.eof()) {
        break;
      }
      continue;
    }
    std::istringstream input_helper(line_buffer);
    auto              &sink = result->outputs.emplace_back();
    input_helper >> sink.output_fd >> sink.include_backtrace >> sink.filter;
  }

  return result;
}