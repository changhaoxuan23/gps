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

#include <cstdio>
#include <process_information.hh>
#include <utils.hh>

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <format>
#include <print>
#include <unistd.h>
namespace {
// format error
//  this function simply formats current errno into
//    [Error <errno>] <description>
inline static auto error_string() -> std::string {
  return std::format("[Error {}] {}", errno, ::strerror(errno));
}

// get a file descriptor about the path of /proc/<pid>
//  the ownership of file descriptor returned is transferred to the caller
//  the file descriptor will have O_PATH, O_CLOEXEC and O_DIRECTORY set
//  in case of failure, -1 is returned as the standard invalid file descriptor
//  a failure may be caused by:
//   1. /proc is not mounted
//   2. /proc/<pid> does not exist
static auto get_proc_directory(pid_t pid) -> int {
  struct fd_holder {
    int fd;
    fd_holder() {
      this->fd = ::open("/proc", O_PATH | O_CLOEXEC | O_DIRECTORY);
      if (this->fd == -1) {
        std::println(stderr, "cannot open /proc ({}). Is procfs mounted?", error_string());
      }
    }
    ~fd_holder() {
      if (this->fd != -1) {
        ::close(this->fd);
      }
    }
  };

  static fd_holder proc_fd;
  if (proc_fd.fd == -1) {
    return -1;
  }
  auto pid_string = std::to_string(pid);
  return ::openat(proc_fd.fd, pid_string.c_str(), O_PATH | O_CLOEXEC | O_DIRECTORY);
}
} // namespace

namespace GPS {
process_information::host_device::host_device(
  const device_information &device, unsigned long long memory_used
)
  : device(device), memory_used(memory_used) {}

void process_information::uids::fill(const std::smatch &match) {
  this->real_uid         = std::stoul(match[1]);
  this->real_login       = to_username(this->real_uid);
  this->effective_uid    = std::stoul(match[2]);
  this->effective_login  = to_username(this->effective_uid);
  this->saved_uid        = std::stoul(match[3]);
  this->saved_login      = to_username(this->saved_uid);
  this->filesystem_uid   = std::stoul(match[4]);
  this->filesystem_login = to_username(this->filesystem_uid);
}

void process_information::timing::fill(
  unsigned long utime, unsigned long stime, unsigned long long starttime
) {
  const static long clock_ticks = sysconf(_SC_CLK_TCK);
  this->usermode_seconds        = utime / clock_ticks;
  this->kernelmode_seconds      = stime / clock_ticks;
  struct timespec tm;
  clock_gettime(CLOCK_MONOTONIC, &tm);
  this->elapsed_seconds = tm.tv_sec - starttime / clock_ticks;
}

process_information::process_information(pid_t pid) : pid(pid) {
  // open /proc/<pid>
  auto process_fd = get_proc_directory(pid);
  if (process_fd == -1) {
    std::println(stderr, "cannot open /proc/{}: {}", pid, error_string());
    return;
  }

  { // get cpu memory
    auto fd = openat(process_fd, "statm", O_RDONLY);
    if (fd == -1) {
      std::println(stderr, "cannot get CPU memory about pid {}: {}", pid, error_string());
    } else {
      FILE *file = fdopen(fd, "r");
      // see man page proc_pid_statm(5) about how this file should be parsed
      fscanf(file, "%*u%llu", std::addressof(this->cpu_memory));
      this->cpu_memory *= get_pagesize();
      fclose(file);
    }
  }

  { // get command line
    auto fd = openat(process_fd, "cmdline", O_RDONLY);
    this->cmdline_tailing_null_bytes = 0;
    if (fd == -1) {
      std::println(stderr, "cannot get commandline of pid {}: {}", pid, error_string());
    } else {
      FILE *file = fdopen(fd, "r");

      // To be precise, this counts the number of tokens we have seen excluding empty strings after the last
      //  non-empty one. Take these as examples:
      // "foo" "bar"             --> collected_tokens == 2
      // "foo" "bar" "" ""       --> collected_tokens == 2
      // "foo" "bar" "" "foobar" --> collected_tokens == 4
      size_t collected_tokens = 0;

      while (true) {
        // Tokens in commandline are separated by null bytes. However, some processes will erase (zero out)
        //  their commandline, which leaves an instance of "" for each byte cleared since each of which is
        //  recognized as an empty string.
        // Therefore, we count the null bytes after the last token that is not an empty string and omit the
        //  generation of "" in collected commandline
        size_t buffer_size = 0;
        char  *buffer      = nullptr;
        auto   string_size = ::getdelim(&buffer, &buffer_size, '\0', file);
        if (string_size == -1) {
          free(buffer);
          break;
        }
        // return value of getdelim is number of bytes read including the delimiter
        //  substract one from it to get the real string size
        string_size -= 1;
        std::string token(buffer, string_size);
        free(buffer);
        this->args.emplace_back(std::move(token));
        if (string_size != 0) {
          collected_tokens = this->args.size();
        }
      }
      this->cmdline_tailing_null_bytes = this->args.size() - collected_tokens;
      this->args.resize(collected_tokens);
      fclose(file);
    }
  }

  { // get uid information
    int fd = openat(process_fd, "status", O_RDONLY);
    if (fd == -1) {
      std::println(stderr, "cannot get credentials of pid {}: {}", pid, error_string());
    } else {
      FILE *file = fdopen(fd, "r");
      while (true) {
        size_t buffer_size = 0;
        char  *buffer      = nullptr;
        auto   string_size = ::getline(&buffer, &buffer_size, file);
        if (string_size == -1) {
          free(buffer);
          break;
        }
        std::string line(buffer, string_size - 1);
        free(buffer);
        if (line.size() != 0) {
          static std::regex matcher(
            R"(^\s*uid:\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*$)", std::regex::ECMAScript | std::regex::icase
          );
          std::smatch result;
          std::regex_match(line, result, matcher);
          if (!result.empty()) {
            this->uids.fill(result);
            break;
          }
        }
      }
      fclose(file);
    }
  }

  { // get timing
    int fd = openat(process_fd, "stat", O_RDONLY);
    if (fd == -1) {
      std::println(stderr, "cannot get timing information of pid {}: {}", pid, error_string());
    } else {
      FILE              *file = fdopen(fd, "r");
      unsigned long      utime, stime;
      unsigned long long starttime;
      // see man page proc_pid_stat(5) about how this file should be parsed
      fscanf(
        file,
        "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %*d %*d %*d %*d %llu",
        std::addressof(utime),
        std::addressof(stime),
        std::addressof(starttime)
      );
      this->timing.fill(utime, stime, starttime);
      fclose(file);
    }
  }

  close(process_fd);
}
} // namespace GPS