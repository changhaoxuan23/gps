// hooked_api_config.hh - configuring interface to the hooked CUDA APIs
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
#ifndef GPS_HOOKED_API_CONFIG_HH_
#define GPS_HOOKED_API_CONFIG_HH_
#include <memory>
#include <string>
#include <vector>
// configuration about one output sink
struct GPSHookedAPIOutputConfiguration {
  // file descriptor to which shall output being written to
  int         output_fd;
  // if backtrace should be printed alongside with the call
  bool        include_backtrace;
  // filter of API name: only calls to APIs whose whole name matches this regular expression will be
  //  reported to this sink.  The string is matched in case sensitive way. empty string means filtering
  //  is disabled for this sink, all calls should be reported.
  std::string filter;
};
// structure containing configurations
struct GPSHookedAPIConfiguration {
  // sinks
  std::vector<GPSHookedAPIOutputConfiguration> outputs;
};

// prepare for launching process with hooked API from this process
//  after this function returns, the environment variables have been set properly so that
//   - processes launched from this process and processes forked from this process without changed environment
//      will have their accessible CUDA API hooked
//   - configuration supplied as argument will be available to the hooked APIs in (forked and) exec'ed process
//  calling this function multiples times, even across fork/exec boundary, is acceptable as long as the same
//   config is not supplied multiple times: the same output sink will not be merged but duplicated, causing
//   unspecified behaviour
//  generally, this function makes it possible to chaining request to hook the API:
//   you may want glaunch to watch the memory consumption while gtrace report all CUDA API calls
//   with help from this function, this can be done as simple as chaining glaunch gtrace <program>
//   without making the code overly complicated
void setup_api_hook(GPSHookedAPIConfiguration &&config);

auto get_hooked_api_configuration() -> std::unique_ptr<GPSHookedAPIConfiguration>;
#endif