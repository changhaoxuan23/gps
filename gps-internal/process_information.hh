// gps-internal:process_information - Structure that holds information about a process
// availability Copyright (C) 2025 Haoxuan Chang<changhaoxuan23@mails.ucas.ac.cn>

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

#ifndef GPS_GPS_INTERNAL_PROCESS_INFORMATION_HH_
#define GPS_GPS_INTERNAL_PROCESS_INFORMATION_HH_
#include <nvml_common.hh>

#include <regex>
#include <string>
#include <sys/types.h>
#include <vector>
namespace GPS {
struct process_information {
  // information about device that this process is running on
  struct host_device {
    // the device information
    const device_information &device;
    // memory used on the device, measured by bytes
    unsigned long long        memory_used;

    host_device(const device_information &device, unsigned long long memory_used);
  };
  struct uids {
    uid_t       real_uid;
    std::string real_login;
    uid_t       effective_uid;
    std::string effective_login;
    uid_t       saved_uid;
    std::string saved_login;
    uid_t       filesystem_uid;
    std::string filesystem_login;

    void fill(const std::smatch &match);
  };
  struct timing {
    unsigned long      usermode_seconds;
    unsigned long      kernelmode_seconds;
    unsigned long long elapsed_seconds;

    void fill(unsigned long utime, unsigned long stime, unsigned long long starttime);
  };

  pid_t                    pid;
  std::vector<host_device> devices;
  unsigned long long       cpu_memory;

  // collected commandline of the process
  std::vector<std::string> args;
  // number of null bytes in the commandline, after the terminator of the last non-empty string
  size_t                   cmdline_tailing_null_bytes;

  uids                     uids;
  timing                   timing;

  process_information(pid_t pid);
};
} // namespace GPS
#endif