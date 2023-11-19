// gps - list all compute processes running on gpu with detailed information
// Copyright (C) 2023 Haoxuan Chang<changhaoxuan23@mails.ucas.ac.cn>

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
#include <array>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <nvml.h>
#include <pwd.h>
#include <regex>
#include <string>
#include <sys/types.h>
#include <unistd.h>
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
struct process_information {
private:
  static auto get_pagesize() -> unsigned int {
    static unsigned int page_size = 0;
    if (page_size == 0) {
      page_size = sysconf(_SC_PAGESIZE);
    }
    return page_size;
  }

public:
  // information about device that this process is running on
  struct host_device {
    unsigned int id;                // index of the device
    unsigned long long memory_used; // memory used on the device, measured by bytes

    host_device(unsigned int id, unsigned long long memory_used) : id(id), memory_used(memory_used) {}
  };
  struct uids {
  private:
    static auto get_username(uid_t uid) -> std::string {
      struct passwd *pwd = getpwuid(uid);
      if (pwd == nullptr) {
        fprintf(stderr, "failed to get username for uid %u\n", uid);
        return std::to_string(uid);
      }
      return pwd->pw_name;
    }

  public:
    uid_t real_uid;
    std::string real_login;
    uid_t effective_uid;
    std::string effective_login;
    uid_t saved_uid;
    std::string saved_login;
    uid_t filesystem_uid;
    std::string filesystem_login;

    void fill(const std::smatch &match) {
      this->real_uid = std::stoul(match[1]);
      this->real_login = get_username(this->real_uid);
      this->effective_uid = std::stoul(match[2]);
      this->effective_login = get_username(this->effective_uid);
      this->saved_uid = std::stoul(match[3]);
      this->saved_login = get_username(this->saved_uid);
      this->filesystem_uid = std::stoul(match[4]);
      this->filesystem_login = get_username(this->filesystem_uid);
    }
  };
  struct timing {
    unsigned long usermode_seconds;
    unsigned long kernelmode_seconds;
    unsigned long long elapsed_seconds;

    void fill(unsigned long utime, unsigned long stime, unsigned long long starttime) {
      const static long clock_ticks = sysconf(_SC_CLK_TCK);
      this->usermode_seconds = utime / clock_ticks;
      this->kernelmode_seconds = stime / clock_ticks;
      struct timespec tm;
      clock_gettime(CLOCK_MONOTONIC, &tm);
      this->elapsed_seconds = tm.tv_sec - starttime / clock_ticks;
    }
  };

  unsigned int pid;
  std::vector<host_device> devices;
  unsigned long long cpu_memory;
  std::vector<std::string> args;
  uids uids;
  timing timing;

  process_information(unsigned int pid) : pid(pid) {
    // open /proc
    DIR *proc = opendir("/proc");
    if (proc == nullptr) {
      perror("opendir /proc");
    } else {
      do {
        // open /proc/[PID]
        int proc_fd = dirfd(proc);
        int process_fd = openat(proc_fd, std::to_string(this->pid).c_str(), O_DIRECTORY | O_RDONLY);
        if (process_fd == -1) {
          perror((std::string("open process /proc/") + std::to_string(this->pid)).c_str());
          break;
        }

        // get cpu memory
        int statm_fd = openat(process_fd, "statm", O_RDONLY);
        if (statm_fd == -1) {
          fprintf(stderr, "[pid=%u] ", this->pid);
          perror("cannot get memory information");
        } else {
          FILE *file = fdopen(statm_fd, "r");
          fscanf(file, "%*u%llu", std::addressof(this->cpu_memory));
          this->cpu_memory *= get_pagesize();
          fclose(file);
          statm_fd = -1;
        }

        // get command line
        int cmdline_fd = openat(process_fd, "cmdline", O_RDONLY);
        if (cmdline_fd == -1) {
          fprintf(stderr, "[pid=%u] ", this->pid);
          perror("cannot get command line");
        } else {
          FILE *file = fdopen(cmdline_fd, "r");
          std::string buffer;
          while (true) {
            buffer.clear();
            while (true) {
              int c = fgetc(file);
              if (feof(file) || c == '\0') {
                break;
              }
              buffer.push_back(static_cast<decltype(buffer)::value_type>(c));
            }
            if (buffer.size() != 0) {
              this->args.push_back(buffer);
            } else {
              break;
            }
          }
          fclose(file);
          cmdline_fd = -1;
        }

        // get uid information
        int status_pid = openat(process_fd, "status", O_RDONLY);
        if (status_pid == -1) {
          fprintf(stderr, "[pid=%u] ", this->pid);
          perror("cannot get status");
        } else {
          FILE *file = fdopen(status_pid, "r");
          std::string buffer;
          while (true) {
            buffer.clear();
            while (true) {
              int c = fgetc(file);
              if (feof(file) || c == '\n') {
                break;
              }
              buffer.push_back(static_cast<decltype(buffer)::value_type>(c));
            }
            if (buffer.size() != 0) {
              static std::regex matcher(
                  R"(^\s*uid:\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*$)",
                  std::regex::ECMAScript | std::regex::icase
              );
              std::smatch result;
              std::regex_match(buffer, result, matcher);
              if (result.empty()) {
                continue;
              } else {
                this->uids.fill(result);
                break;
              }

            } else {
              break;
            }
          }
          fclose(file);
          status_pid = -1;
        }

        // get timing
        int stat_fd = openat(process_fd, "stat", O_RDONLY);
        if (stat_fd == -1) {
          fprintf(stderr, "[pid=%u] ", this->pid);
          perror("cannot get stat");
        } else {
          FILE *file = fdopen(stat_fd, "r");
          unsigned long utime, stime;
          unsigned long long starttime;
          fscanf(
              file,
              "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %*d %*d %*d %*d %llu",
              std::addressof(utime), std::addressof(stime), std::addressof(starttime)
          );
          this->timing.fill(utime, stime, starttime);
          fclose(file);
          stat_fd = -1;
        }

        close(process_fd);
      } while (false);
      closedir(proc);
      proc = nullptr;
    }
  }
};
inline auto get_readable_size(unsigned long long value) -> std::string {
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
  value = static_cast<unsigned long long>(round(temporary));
  return std::to_string(value) + suffixes[selection];
}
inline auto get_readable_duration(unsigned long long seconds) -> std::string {
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
auto main() -> int {
  fprintf(stdout, "gps v0.0.1 licensed under AGPLv3 or later\n");
  fprintf(stdout, "you can goto https://github.com/changhaoxuan23/gps for source code\n\n");
  panic_on_failure(nvmlInit_v2);
  unsigned int device_count = 0;
  panic_on_failure(nvmlDeviceGetCount_v2, &device_count);
  std::map<unsigned int, process_information> processes;
  std::vector<device_information> devices;
  devices.reserve(device_count);
  for (unsigned int i = 0; i < device_count; i++) {
    nvmlDevice_t device = nullptr;
    nvmlReturn_t return_value = nvmlDeviceGetHandleByIndex_v2(i, &device);
    if (return_value != NVML_SUCCESS) {
      fprintf(stderr, "failed to open device %u: %s, skipping.\n", i, nvmlErrorString(return_value));
      continue;
    }
    // gather information about this device
    devices.emplace_back(device);
    // query number of compute processes running on this process
    unsigned int process_count = 0;
    nvmlProcessInfo_t *information = nullptr;
    while (true) {
      // since the number of process may change, we need to loop and keep increasing the size of buffer until
      //  we can finally during some call to the API have sufficient space for all processes running
      return_value = nvmlDeviceGetComputeRunningProcesses(device, &process_count, information);
      if (return_value != NVML_ERROR_INSUFFICIENT_SIZE) {
        if (return_value != NVML_SUCCESS) {
          fprintf(stderr, "failed to query device %u: %s, skipping.\n", i, nvmlErrorString(return_value));
          free(information);
          information = nullptr;
        }
        break;
      } else {
        void *new_buffer = realloc(information, sizeof(nvmlProcessInfo_t) * process_count);
        if (new_buffer == nullptr) {
          // oops, looks like something really bad happened
          perror("failed to allocate memory");
          free(information);
          information = nullptr;
          break;
        }
        information = static_cast<nvmlProcessInfo_t *>(new_buffer);
      }
    }
    if (information == nullptr) {
      // we encountered some problem, skip further queries to this device
      continue;
    }
    for (unsigned int process_index = 0; process_index < process_count; process_index++) {
      auto iter = processes.find(information[process_index].pid);
      if (iter == processes.end()) {
        // this process is not yet recorded
        iter = processes.emplace(information[process_index].pid, information[process_index].pid).first;
      }
      iter->second.devices.emplace_back(i, information[process_index].usedGpuMemory);
    }
    free(information);
    information = nullptr;
  }
  for (const auto &[pid, information] : processes) {
    printf("[%u] ", pid);
    if (information.args.size() == 0) {
      printf("unknown command line\n");
    } else {
      for (const auto &part : information.args) {
        printf("\'%s\' ", part.c_str());
      }
      putchar('\n');
    }

    // owner/permission information
    printf("[%u]   Owner:\n", pid);
    printf(
        "[%u]     Effective UID:  %u (%s)\n", pid, information.uids.effective_uid,
        information.uids.effective_login.c_str()
    );
    printf(
        "[%u]     Real UID:       %u (%s)\n", pid, information.uids.real_uid,
        information.uids.real_login.c_str()
    );
    printf(
        "[%u]     Saved UID:      %u (%s)\n", pid, information.uids.saved_uid,
        information.uids.saved_login.c_str()
    );
    printf(
        "[%u]     Filesystem UID: %u (%s)\n", pid, information.uids.filesystem_uid,
        information.uids.filesystem_login.c_str()
    );

    // timing information
    printf("[%u]   Timing:\n", pid);
    printf(
        "[%u]     Usermode:    %lu second(s) (%s)\n", pid, information.timing.usermode_seconds,
        get_readable_duration(information.timing.usermode_seconds).c_str()
    );
    printf(
        "[%u]     Kernelmode:  %lu second(s) (%s)\n", pid, information.timing.kernelmode_seconds,
        get_readable_duration(information.timing.kernelmode_seconds).c_str()
    );
    printf(
        "[%u]     Wall-clock:  %llu second(s) (%s)\n", pid, information.timing.elapsed_seconds,
        get_readable_duration(information.timing.elapsed_seconds).c_str()
    );

    // CPU information
    printf("[%u]   CPU memory: %s\n", pid, get_readable_size(information.cpu_memory).c_str());

    // GPU information
    unsigned long long total_gpu_memory = 0;
    for (const auto device : information.devices) {
      total_gpu_memory += device.memory_used;
    }
    printf(
        "[%u]   GPU memory: running on %lu devices, %s in use\n", pid, information.devices.size(),
        get_readable_size(total_gpu_memory).c_str()
    );
    for (const auto device : information.devices) {
      const auto &device_information = devices.at(device.id);
      printf(
          "[%u]     on device %u (%s): %s / %s, %.3lf%%\n", pid, device_information.id,
          device_information.name.c_str(), get_readable_size(device.memory_used).c_str(),
          get_readable_size(device_information.memory.total).c_str(),
          static_cast<double>(device.memory_used) / static_cast<double>(device_information.memory.total) * 100
      );
    }
    putchar('\n');
  }
  nvmlShutdown();
  return 0;
}