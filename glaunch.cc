// glaunch - Launch computational process on proper GPUs regards to memory
// availability Copyright (C) 2023-2025 Haoxuan Chang<changhaoxuan23@mails.ucas.ac.cn>

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

#include <configuration.hh>
#include <hooked_api_config.hh>
#include <nvml_common.hh>

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>
#ifdef HAVE_SECRET_STORAGE
#include <secret_storage_accessor.hh>
#endif

struct MemoryMonitoringMethod {
public:
  using value_type = uint16_t;
  enum : value_type {
    NONE  = 0x0000,
    NVML  = 0x0001,
    Trace = 0x0002,
    ALL   = 0xffff,
  };
  using enum_type = decltype(MemoryMonitoringMethod::NONE);

  // interval between two queries using the NVML library
  //  this is only effective if NVML is set while Trace is not
  uint32_t nvml_query_interval;

  MemoryMonitoringMethod() = default;
  explicit MemoryMonitoringMethod(enum_type value) : value(value) {}
  auto operator=(enum_type rhs) -> MemoryMonitoringMethod & {
    this->value = rhs;
    return *this;
  }
  operator bool() const { return this->value != MemoryMonitoringMethod::NONE; }
  auto operator|(enum_type rhs) const -> MemoryMonitoringMethod {
    return MemoryMonitoringMethod(static_cast<enum_type>(this->value | rhs));
  }
  auto operator|(const MemoryMonitoringMethod &rhs) const -> MemoryMonitoringMethod {
    return this->operator|(rhs.value);
  }
  auto operator&(enum_type rhs) const -> MemoryMonitoringMethod {
    return MemoryMonitoringMethod(static_cast<enum_type>(this->value & rhs));
  }
  auto operator&(const MemoryMonitoringMethod &rhs) const -> MemoryMonitoringMethod {
    return this->operator&(rhs.value);
  }
  auto operator~() const -> MemoryMonitoringMethod {
    return MemoryMonitoringMethod(static_cast<enum_type>(MemoryMonitoringMethod::ALL - this->value));
  }
  auto operator|=(enum_type rhs) -> MemoryMonitoringMethod & {
    this->value = static_cast<enum_type>(this->value | rhs);
    return *this;
  }
  auto operator|=(const MemoryMonitoringMethod &rhs) -> MemoryMonitoringMethod & {
    return this->operator|=(rhs.value);
  }
  auto operator&=(enum_type rhs) -> MemoryMonitoringMethod & {
    this->value = static_cast<enum_type>(this->value & rhs);
    return *this;
  }
  auto operator&=(const MemoryMonitoringMethod &rhs) -> MemoryMonitoringMethod & {
    return this->operator&=(rhs.value);
  }

private:
  enum_type value{MemoryMonitoringMethod::NONE};
};
template <> struct std::formatter<MemoryMonitoringMethod> {
  constexpr auto parse(std::format_parse_context &ctx) { return ctx.begin(); }
  template <std::output_iterator<char> Iter>
  auto format(const MemoryMonitoringMethod &v, std::basic_format_context<Iter, char> &ctx) const {
    constexpr auto keys  = std::to_array<std::pair<const char *, MemoryMonitoringMethod::enum_type>>({
      {"nvml", MemoryMonitoringMethod::NVML},
      {"trace", MemoryMonitoringMethod::Trace},
    });
    auto         &&out   = ctx.out();
    bool           first = true;
    for (const auto [name, value] : keys) {
      if (!(v & value)) {
        continue;
      }
      if (first) {
        first = false;
      } else {
        std::format_to(out, " | ");
      }
      std::format_to(out, "{}", name);
    }
    if (first) {
      std::format_to(out, "NONE");
    }
    return out;
  }
};

struct Options {
  enum class SelectionPolicy { BestFit, WorstFit };

  // the program to be launched shall be started in background
  //  to be specific, it shall be forked and detached from the current controlling shell with output closed
  bool background;

  // the program launched shall be timed
  bool            timing;
  uint16_t        gpu_count;
  SelectionPolicy policy;
  uint32_t        wait_memory_timeout;
  std::string     logging_path;
  size_t          break_point;

  // the method to watch gpu memory usage
  MemoryMonitoringMethod monitor_gpu_memory;
  size_t                 memory_estimation;
#ifdef HAVE_SECRET_STORAGE
  std::string email_server;
  uint16_t    email_port;
#endif

  // check if we should execute the program to start directly without forking first
  //  if a direct exec is done, we will not have our process alongside with the program
  //  launched and will have limited capabilities.
  [[nodiscard]] auto direct_exec() const -> bool {
    return !this->timing // we need to record and report the timepoint the program is launched and the
                         // timepoint which terminates if we are doing timing
        && !this->monitor_gpu_memory // we need to query with NVML or process traced API calls to
                                     // monitor GPU memory usage
#ifdef HAVE_SECRET_STORAGE
        && this->email_port == 0 // we need to send email after the program launched terminates
#endif
      ;
  };

  Options(std::unordered_map<std::string, std::any> parse_result) {
    if (parse_result.contains("gpus")) {
      this->gpu_count = std::any_cast<uint16_t>(parse_result.at("gpus"));
    } else {
      this->gpu_count = 1;
    }

    if (parse_result.contains("memory-budget")) {
      this->memory_estimation = std::any_cast<unsigned long long>(parse_result.at("memory-budget"));
    } else {
      this->memory_estimation = 0;
    }

    if (parse_result.contains("policy")) {
      this->policy = std::any_cast<Options::SelectionPolicy>(parse_result.at("policy"));
    } else {
      this->policy = SelectionPolicy::WorstFit;
    }

    this->timing     = parse_result.contains("time");
    this->background = parse_result.contains("background");

    if (parse_result.contains("log")) {
      this->logging_path = std::any_cast<std::string>(parse_result.at("log"));
    } else {
      this->logging_path = "";
    }

    if (parse_result.contains("watch-memory")) {
      const auto      &list_ = std::any_cast<std::string>(parse_result.at("watch-memory"));
      std::string_view list{list_};
      size_t           start = 0;
      while (true) {
        auto end   = list.find_first_of(',', start);
        auto entry = list.substr(start, end - start);
        if (entry == "all") {
          this->monitor_gpu_memory = MemoryMonitoringMethod::ALL;
        } else if (entry.starts_with("nvml")) {
          this->monitor_gpu_memory |= MemoryMonitoringMethod::NVML;
          if (entry.size() > 5 && entry[4] == ':') {
            std::string               temporary{entry.substr(5)};
            std::span<std::string, 1> helper{&temporary, 1};
            this->monitor_gpu_memory.nvml_query_interval = std::any_cast<unsigned long long>(
              Configurations::CommonParsers::duration_parser(helper.begin(), helper.end())
            );
          } else {
            this->monitor_gpu_memory.nvml_query_interval = 5;
          }
        } else if (entry == "trace") {
          this->monitor_gpu_memory |= MemoryMonitoringMethod::Trace;
        } else {
          std::println(stderr, "invalid type supplied to watch-memory: `{}'", entry);
          ::exit(EXIT_FAILURE);
        }
        if (end == entry.npos) {
          break;
        }
        start = end + 1;
      }
    } else {
      this->monitor_gpu_memory = MemoryMonitoringMethod::NONE;
    }

    if (parse_result.contains("wait-timeout")) {
      this->wait_memory_timeout = std::any_cast<unsigned long long>(parse_result.at("wait-timeout"));
    } else {
      this->wait_memory_timeout = 0;
    }

#ifdef HAVE_SECRET_STORAGE
    if (parse_result.contains("email-notify")) {
      auto server_string = std::any_cast<std::string>(parse_result.at("email-notify"));
      if (server_string.front() == '[') {
        auto stop_point = server_string.find(']');
        if (stop_point == server_string.npos) {
          std::println(stderr, "invalid server string: unmatched '['");
          exit(EXIT_FAILURE);
        }
        if (stop_point == server_string.size() - 1) {
          this->email_server = server_string.substr(1, server_string.size() - 2);
          this->email_port   = 465;
        } else {
          if (server_string.size() < stop_point + 3) {
            std::println(stderr, "invalid server string: insufficient length of port number");
            exit(EXIT_FAILURE);
          }
          if (server_string[stop_point + 1] != ':') {
            std::println(stderr, "invalid server string: expected ':' before port number");
            exit(EXIT_FAILURE);
          }
          this->email_port = Configurations::CommonParsers::full_convert_unsigned<uint16_t>(
            server_string.substr(stop_point + 2)
          );
          this->email_server = server_string.substr(1, stop_point - 1);
        }
      } else {
        if (server_string.find(':') != server_string.rfind(':')) {
          std::println(
            stderr, "invalid server string: more than one ':' detected, enclose IPv6 address with []"
          );
          exit(EXIT_FAILURE);
        }
        auto stop_point = server_string.find(':');
        if (stop_point == server_string.npos) {
          this->email_server = server_string;
          this->email_port   = 465;
        } else {
          this->email_port = Configurations::CommonParsers::full_convert_unsigned<uint16_t>(
            server_string.substr(stop_point + 1)
          );
          this->email_server = server_string.substr(0, stop_point);
        }
      }
    } else {
      this->email_server = "";
      this->email_port   = 0;
    }
#endif

    this->break_point = std::any_cast<size_t>(parse_result.at("_break_point"));
  }
  void dump(FILE *file) const {
    std::println(file, "------ options dump ------");
    std::println(file, "background   : {}", this->background ? "true" : "false");
    std::println(file, "break_point  : {}", this->break_point);
#ifdef HAVE_SECRET_STORAGE
    std::println(file, "email_server : {}", this->email_server);
    std::println(file, "email_port   : {}", this->email_port);
#endif
    std::println(file, "gpu_count    : {}", this->gpu_count);
    std::println(file, "logging_path : {}", this->logging_path);
    std::println(file, "memory_budget: {}", this->memory_estimation);
    std::println(file, "policy       : {}Fit", this->policy == SelectionPolicy::BestFit ? "Best" : "Worst");
    std::println(file, "timing       : {}", this->timing ? "true" : "false");
    std::println(file, "wait_timeout : {}", this->wait_memory_timeout);
    std::println(file, "watch_memory : {}", this->monitor_gpu_memory);
    std::println(file, "------ options dump ------");
  }
};
class Parser : public Configurations {
public:
  Parser() {
    this->add_option(
      "--gpus",
      [](parser_argument_iterator_t begin, parser_argument_iterator_t) -> std::any {
        return CommonParsers::full_convert_unsigned<uint16_t>(*begin);
      },
      1
    );
    this->add_option("--memory-budget", CommonParsers::size_parser, 1);
    this->add_option(
      "--policy",
      [](parser_argument_iterator_t begin, parser_argument_iterator_t) -> std::any {
        if (*begin == "WorstFit") {
          return Options::SelectionPolicy::WorstFit;
        }
        if (*begin == "BestFit") {
          return Options::SelectionPolicy::BestFit;
        }
        std::println(stderr, "invalid option for --policy: {}", *begin);
        exit(EXIT_FAILURE);
      },
      1
    );
    this->add_option("--time", CommonParsers::true_parser, 0);
    this->add_option("--background", CommonParsers::true_parser, 0);
    this->add_option("--log", CommonParsers::identity_parser, 1);
    this->add_option("--watch-memory", CommonParsers::identity_parser, 1);
    this->add_option("--wait-timeout", CommonParsers::duration_parser, 1);
#ifdef HAVE_SECRET_STORAGE
    this->add_option("--email-notify", CommonParsers::identity_parser, 1);
#endif
  }
  void help() const override {
    std::println("Launch computational process on proper GPUs regards to memory availability              ");
    std::println("Usage: glaunch [OPTIONS...] [--] PROGRAM [ARGS...]                                      ");
    std::println("OPTIONS:                                                                                ");
    std::println("  --gpus COUNT               Use COUNT gpus for this program, defaults to 1             ");
    std::println("                                                                                        ");
    std::println("  --memory-budget SIZE       Slightly over-estimated size of memory your program will   ");
    std::println("                              consume per GPU. Suffixes are allowed to simplify this    ");
    std::println("                              configuration, try KiB, MiB, GiB, etc.. If you do not     ");
    std::println("                              specify such value, we assume that your program could     ");
    std::println("                              run with arbitrary amount of memory                       ");
    std::println("                                                                                        ");
    std::println("  --policy POLICY            Policy used to select GPU devices. Currently two policies  ");
    std::println("                              are supported while we defaults to the first one:         ");
    std::println("                               WorstFit: MAXIMIZE free space after your program launches");
    std::println("                               BestFit: MINIMIZE free space after your program launches ");
    std::println("                                                                                        ");
    std::println("  --time                     When the program terminates, summary its elapsed time      ");
    std::println("                                                                                        ");
    std::println("  --background               Return after the process is launched, effectively run it   ");
    std::println("                              in background. This will also remap file descriptors so   ");
    std::println("                              that all output from the program will be discarded.       ");
    std::println("                             A program launched in background mode will keep running    ");
    std::println("                              even after you closed the terminal and logged out.        ");
    std::println("                             See also --log.                                            ");
    std::println("                                                                                        ");
    std::println("  --log PATH                 Duplicate and save stdout and stderr to PATH               ");
    std::println("                                                                                        ");
    std::println("  --watch-memory TYPE        Dump GPU memory usage. TYPE should be a list separated by  ");
    std::println("                              comma to instruct how GPU memory usage shall be measured. ");
    std::println("                              No space shall be included in the list.                   ");
    std::println("                              Possible values are:                                      ");
    std::println("                               nvml: measure with NVML, the NVIDIA management library.  ");
    std::println("                                     this shall report identical value as nvidia-smi.   ");
    std::println("                                     If this option is supplied together with trace, the");
    std::println("                                     GPU memory usage will be measured whenever an event");
    std::println("                                     is reported in the trace. Otherwise it will be done");
    std::println("                                     by polling, in which case the interval defaults to ");
    std::println("                                     5 seconds while may be configured by suffixing this");
    std::println("                                     key with a colon and the DURATION specification.   ");
    std::println("                                     Example:                                           ");
    std::println("                                      nvml:1m will cause the memory usage being measured");
    std::println("                                       and reported every one minute.                   ");
    std::println("                               trace: measure by tracing all CUDA runtime APIs.         ");
    std::println("                                      this will record all call to CUDA runtime API that");
    std::println("                                       allocates or frees memory on device, providing an");
    std::println("                                       accurate measurement on size of memory requested ");
    std::println("                                       by the program while excluding any overhead.     ");
    std::println("                                      note that therefore, memory usage measured can be ");
    std::println("                                       smaller than the one reported by nvml, and such  ");
    std::println("                                       difference can be more significant if the memory ");
    std::println("                                       memory watermark is high, i.e. the process asked ");
    std::println("                                       for only a small amount of memory.               ");
    std::println("                               all: measure by every possible method and report them all");
    std::println("                                                                                        ");
    std::println("  --wait-timeout DURATION    Wait for no more than DURATION if no device have sufficient");
    std::println("                              memory launching the specified process, -1 for infinity.  ");
    std::println("                             Suffixes are supported.                                    ");
    std::println("                                                                                        ");
#ifdef HAVE_SECRET_STORAGE
    std::println("  --email-notify SERVER      Notify about the termination of process launched via email ");
    std::println("                              The address/domain name and port of SMTP server is given  ");
    std::println("                               via SERVER string, which is <address/domain name>[:port] ");
    std::println("                               the address/domain name is required and port is optional.");
    std::println("                              If a IPv6 address is specified, enclose it with [] so that");
    std::println("                               it will not be mistaken as port number.                  ");
    std::println("                              If port number is omitted, we defaults to 465, the default");
    std::println("                               port number of SMTP over TLS.                            ");
    std::println("                              Attention: we do not check the address supplied but will  ");
    std::println("                               pass it directly to the email sender.                    ");
    std::println("                              Note that due to security consideration, we support only  ");
    std::println("                               SMTP over TLS.                                           ");
    std::println("                              Address and password is designed to be asked interactively");
    std::println("                               to stop it from leaking from command line arguments which");
    std::println("                               is usually visible to any other user on the same machine,");
    std::println("                               and stored in a secret-storage server until the launched ");
    std::println("                               process terminates, to a running secret-storage server is");
    std::println("                               required for this to function correctly.                 ");
    std::println("                              The notification will contain the arguments used to launch");
    std::println("                               the process, its pid, exit status and following fields:  ");
    std::println("                               if --time is specified, total wall-clock time it costs;  ");
    std::println("                               if --log is specified, the logging file as an attachment.");
    std::println("                                                                                        ");
#endif
    std::println("  --help                     Show this message again                                    ");
    std::println("                                                                                        ");
    std::println("                                                                                        ");
    std::println("PROGRAM: the program to launch                                                          ");
    std::println("ARGS: arguments passed to PROGRAM which will not be modified                            ");
    std::println("                                                                                        ");
    std::println("Notes:                                                                                  ");
    std::println("  There are several options that sets up a DURATION which will be parsed as either a    ");
    std::println("   timeout or an interval. Due to task scheduling and querying/calculating overheads,   ");
    std::println("   do not expect them to be perfectly accurate: minor mistake will occur.               ");
    std::println("  If you got some trouble on argument parsing, which may be triggered by a program      ");
    std::println("   whose name starts with '--', you can add '--' before it to terminate option parsing  ");
    std::println("   for example, glaunch --time -- --your-program                                        ");
  }
};

class EnvironmentArranger {
public:
  EnvironmentArranger()          = default;
  virtual ~EnvironmentArranger() = default;
  virtual void before_fork()     = 0;
  void         after_fork(pid_t pid) {
    if (pid == 0) {
      this->arrange_children();
    } else {
      this->arrange_parent(pid);
    }
  }

protected:
  virtual void arrange_children()        = 0;
  virtual void arrange_parent(pid_t pid) = 0;
};

class LogEnvironmentArranger : public EnvironmentArranger {
  // setup logging: we use tee to do this job, assuming which is installed on the system
  //  since it is part of the GNU coreutils, it shall be safe to make such assumption in common cases
private:
  int                         pipes[2];
  const std::filesystem::path destination;

protected:
  void arrange_children() override {
    // close and reopen stdout/stderr on the pipe
    if (this->pipes[1] != -1) {
      dup2(this->pipes[1], STDOUT_FILENO);
      dup2(this->pipes[1], STDERR_FILENO);
      close(pipes[1]);
    }
  }
  void arrange_parent(pid_t) override {
    if (this->pipes[1] != -1) {
      close(this->pipes[1]);
    }
  }

public:
  LogEnvironmentArranger(std::filesystem::path destination) : destination(std::move(destination)) {}
  void before_fork() override {
    if (pipe(this->pipes) == -1) {
      perror("failed to make pipe for logging");
      exit(EXIT_FAILURE);
    }
    pid_t pid = fork();
    if (pid == -1) {
      perror("cannot fork to launch tee");
      close(this->pipes[0]);
      close(this->pipes[1]);
      this->pipes[0] = -1;
      this->pipes[1] = -1;
      return;
    }

    if (pid == 0) {
      dup2(this->pipes[0], STDIN_FILENO);
      close_range(3, ~0U, CLOSE_RANGE_UNSHARE);
      execlp("tee", "tee", this->destination.c_str(), nullptr);
      // you shall not be here
      perror("cannot exec tee");
    }
    close(this->pipes[0]);
  }
};

class MemoryWatcherEnvironmentArranger : public EnvironmentArranger {
private:
  int                             pipes[2];
  MemoryMonitoringMethod          method;
  std::vector<device_information> running_devices;
  pid_t                           pid;

  struct parsed_trace_line {
    std::string_view api_name;
    std::string_view arguments;
    bool             succeed;

    parsed_trace_line(std::string_view line) {
      auto name_split     = line.find_first_of('(');
      this->api_name      = line.substr(0, name_split);
      auto argument_split = line.find(") -> ");
      this->arguments     = line.substr(name_split + 1, argument_split - name_split - 1);
      this->succeed       = line[argument_split + 5] == '0';
    }
  };

  static auto get_time() -> std::string {
    time_t     current_time = time(nullptr);
    struct tm *current_tm;
    current_tm = localtime(&current_time);
    std::array<char, 512> buffer;
    strftime(buffer.data(), buffer.size(), "[%EY %B %d %T]", current_tm);
    return {buffer.data()};
  }

  void nvml_memory_watcher() {
    unsigned long long total_memory = 0;
    for (const auto &device : this->running_devices) {
      auto processes = device.get_processes();
      for (const auto &process : processes) {
        if (getpgid(static_cast<pid_t>(process.pid)) == this->pid) {
          total_memory += process.usedGpuMemory;
        }
      }
    }
    std::println(
      stderr, "[watch-memory:nvml]{} {} GPU memory in use", this->get_time(), get_readable_size(total_memory)
    );
  }

  void trace_memory_watcher() {
    FILE                                 *input = fdopen(this->pipes[0], "r");
    std::array<char, 512>                 buffer;
    std::unordered_map<uintmax_t, size_t> memory_blocks;
    size_t                                allocated_size   = 0;
    bool                                  warn_array_calls = true;
    bool                                  warn_async_calls = true;

    while (true) {
      buffer[0] = '\0';
      fgets(buffer.data(), buffer.size(), input);
      std::string_view line{buffer.data()};
      if (line.empty()) {
        break;
      }
      line = line.substr(0, line.size() - 1);
      parsed_trace_line trace(line);
      if (!trace.succeed) {
        continue;
      }
      if (trace.api_name == "cudaFreeHost") {
        continue;
      }

      if (this->method & MemoryMonitoringMethod::NVML) {
        this->nvml_memory_watcher();
      }

      if (trace.api_name.contains("Array")) {
        if (warn_array_calls) {
          warn_array_calls = false;
          std::println(
            stderr,
            "[watch-memory:trace]{} Program is using Array-based API(s) which is not yet supported, memory "
            "accounting will be inaccurate.",
            this->get_time()
          );
        }
        std::println(stderr, "[watch-memory:trace]{} Unsupported call: {}", this->get_time(), line);
        continue;
      }

      if (trace.api_name.contains("Async")) {
        if (warn_async_calls) {
          warn_async_calls = false;
          std::println(
            stderr,
            "[watch-memory:trace]{} Program is using async memory allocation API(s) whose failure may be "
            "reported by later API calls instead of the call itself, which is not captured. All async "
            "allocation are treated according to only the direct return value, therefore memory accounting "
            "can be inaccurate if the allocation actually failed but such failure is reported afterwards.",
            this->get_time()
          );
        }
        std::println(stderr, "[watch-memory:trace]{} Async call: {}", this->get_time(), line);
      }

      if (trace.api_name == "cudaFree" || trace.api_name == "cudaFreeAsync") {
        auto target =
          strtoull(trace.arguments.substr(trace.arguments.find("devPtr=[") + 8).data(), nullptr, 16);
        auto iter = memory_blocks.find(target);
        if (iter == memory_blocks.end()) {
          std::println(
            stderr,
            "[watch-memory:trace]{} program freeing an unrecognized address, this is likely caused by an "
            "unrecorded allocation\n"
            "  The call was {}",
            this->get_time(),
            line
          );
          continue;
        }
        allocated_size -= iter->second;
        std::println(
          stderr,
          "[watch-memory:trace]{} freed {}, {} active",
          this->get_time(),
          get_readable_size(iter->second),
          get_readable_size(allocated_size)
        );
        memory_blocks.erase(iter);
      } else {
        uintmax_t target = 0;
        size_t    size   = 0;
        if (trace.api_name == "cudaMallocFromPoolAsync") {
          target = strtoull(trace.arguments.substr(trace.arguments.find("ptr=[") + 5).data(), nullptr, 16);
          size   = strtoull(trace.arguments.substr(trace.arguments.find("size=") + 5).data(), nullptr, 10);
        } else if (trace.api_name == "cudaMalloc" || trace.api_name == "cudaMallocManaged"
                   || trace.api_name == "cudaMallocAsync") {
          target = strtoull(trace.arguments.substr(trace.arguments.find("devPtr=[") + 8).data(), nullptr, 16);
          size   = strtoull(trace.arguments.substr(trace.arguments.find("size=") + 5).data(), nullptr, 10);
        } else if (trace.api_name == "cudaMalloc3D") {
          target = strtoull(trace.arguments.substr(trace.arguments.find("ptr=[") + 5).data(), nullptr, 16);
          auto width =
            strtoull(trace.arguments.substr(trace.arguments.find("pitch=") + 6).data(), nullptr, 10);
          auto depth =
            strtoull(trace.arguments.substr(trace.arguments.find("depth=") + 6).data(), nullptr, 10);
          auto height =
            strtoull(trace.arguments.substr(trace.arguments.find("height=") + 7).data(), nullptr, 10);
          size = width * depth * height;
        } else if (trace.api_name == "cudaMallocPitch") {
          target = strtoull(trace.arguments.substr(trace.arguments.find("devPtr=[") + 8).data(), nullptr, 16);
          auto width =
            strtoull(trace.arguments.substr(trace.arguments.find("pitch=[") + 7).data(), nullptr, 10);
          auto height =
            strtoull(trace.arguments.substr(trace.arguments.find("height=") + 7).data(), nullptr, 10);
          size = width * height;
        } else {
          std::println(
            stderr,
            "[watch-memory:trace]{} Unexpected API call reported, see the following line for detail.\n  {}",
            this->get_time(),
            line
          );
        }

        memory_blocks.emplace(target, size);
        allocated_size += size;
        std::println(
          stderr,
          "[watch-memory:trace{} allocated {}, {} active",
          this->get_time(),
          get_readable_size(size),
          get_readable_size(allocated_size)
        );
      }
    }
  }

public:
  MemoryWatcherEnvironmentArranger(MemoryMonitoringMethod method, std::vector<device_information> &&devices)
    : method(method), running_devices(devices) {
    this->pipes[0] = -1;
    this->pipes[1] = -1;
    this->pid      = -1;
  }
  void before_fork() override {
    if (this->method & MemoryMonitoringMethod::Trace) {
      if (pipe(this->pipes) == -1) {
        perror("failed to make pipe for hooked api report");
        exit(EXIT_FAILURE);
      }
      GPSHookedAPIConfiguration config;
      auto                     &sink = config.outputs.emplace_back();
      sink.output_fd                 = this->pipes[1];
      sink.include_backtrace         = false;
      sink.filter                    = "(cudaFree|cudaMalloc).*";
      setup_api_hook(std::move(config));
    }
  }

protected:
  void arrange_children() override {
    if (this->pipes[0] != -1) {
      close(this->pipes[0]);
    }
  }
  void arrange_parent(pid_t pid) override {
    this->pid = pid;
    if (this->method & MemoryMonitoringMethod::Trace) {
      close(this->pipes[1]);
      std::thread([this]() { this->trace_memory_watcher(); }).detach();
    } else if (this->method & MemoryMonitoringMethod::NVML) {
      std::thread([this]() {
        while (true) {
          std::this_thread::sleep_for(std::chrono::seconds(this->method.nvml_query_interval));
          this->nvml_memory_watcher();
        }
      }).detach();
    }
  }
};

// launch the actual process: this function will run in the
// process which will, by this function, execvp(2)
//  into the actual process
// if we have forked off can be checked via !config.direct_exec()
static auto do_launch(char *argv[], const Options &config) -> int {
  // set process group id to group processes forked from the actual computing process
  setpgid(0, 0);
  if (!config.direct_exec()) { // arrange tasks that shall be done only if we are forking off here
    // make this process get killed when the controlling glaunch process is
    // killed note that this takes no effect if the program to be executed has
    // set-user-id/set-group-id or
    //  capabilities. If not sure, see prctl(2)
    prctl(PR_SET_PDEATHSIG, SIGKILL);
  }
  // logging the command line to be executed
  fprintf(stderr, "executing: [");
  for (auto i = config.break_point; argv[i] != nullptr; i++) {
    if (i != config.break_point) {
      fprintf(stderr, ", ");
    }
    fputc('\'', stderr);
    for (auto c = argv[i]; *c != '\0'; c++) {
      if (*c == '\'') {
        fprintf(stderr, R"('"'"')");
      } else {
        fputc(*c, stderr);
      }
    }
    fputc('\'', stderr);
  }
  fprintf(stderr, "]...\n");
  execvp(argv[config.break_point], std::addressof(argv[config.break_point]));
  perror("failed to exec");
  return -ENOEXEC;
}
// get devices with sufficient memory, sort in decreasing order of free memory
static auto get_available_devices(const Options &config, std::vector<device_information> &devices)
  -> std::vector<device_information> {
  std::ranges::for_each(devices, [](auto &device) { device.resample(); });
  std::ranges::sort(devices, [](const device_information &lhs, const device_information &rhs) -> bool {
    return lhs.memory.free > rhs.memory.free;
  });
  std::vector<device_information> result;
  std::ranges::copy_if(
    devices,
    std::back_inserter(result),
    [config](const device_information &device) -> bool {
      return config.memory_estimation <= device.memory.free;
    }
  );
  return result;
}

// these can be general purpose utility but currently only used in email handling
#ifdef HAVE_SECRET_STORAGE
struct subprocess {
  pid_t pid;
  FILE *standard_input;
};
// launch a subprocess
static auto launch_subprocess(const std::vector<std::string> &arguments) -> subprocess {
  subprocess         result = {.pid = -1, .standard_input = nullptr};
  std::array<int, 2> pipes;
  if (pipe(pipes.data()) == -1) {
    std::println(stderr, "failed to create pipe: {}", strerror(errno));
    return result;
  }
  result.pid = fork();
  if (result.pid == -1) {
    std::println(stderr, "failed to fork: {}", strerror(errno));
    return result;
  }
  if (result.pid == 0) {
    close(pipes[1]);
    fclose(stdin);
    dup2(pipes[0], STDIN_FILENO);
    stdin = fdopen(STDIN_FILENO, "r");
    std::vector<const char *> translated_arguments;
    translated_arguments.reserve(arguments.size() + 1);
    std::ranges::transform(arguments, std::back_insert_iterator(translated_arguments), [](const auto &item) {
      return item.c_str();
    });
    translated_arguments.emplace_back(nullptr);
    execvp(translated_arguments[0], const_cast<char *const *>(translated_arguments.data()));
    std::println(stderr, "failed to execve: {}", strerror(errno));
  }
  close(pipes[0]);
  result.standard_input = fdopen(pipes[1], "w");
  return result;
}
#endif

auto main(int argc, char *argv[]) -> int {
  std::println("glaunch in gps build v{}, licensed under AGPLv3 or later", GPS_VERSION);
  std::println("you can goto https://github.com/changhaoxuan23/gps for source code\n");
  if (argc == 1) {
    std::println(stderr, "invalid usage, invoke `glaunch --help' to see how to use glaunch.");
    exit(EXIT_FAILURE);
  }

  std::vector<std::string> args(argv, argv + argc);
  Options                  config(Parser().parse(args));
  config.dump(stdout);

#ifdef HAVE_SECRET_STORAGE
  // deal with this at the very beginning since this requires active interaction with the user
  std::string_view encoded_email_key;
  std::string_view encoded_password_key;
  if (config.email_port != 0) {
    // make keys
    auto email_key       = SecretStorageAccessor::make_secured_key(64);
    encoded_email_key    = SecretStorageAccessor::encode_string(email_key);
    auto password_key    = SecretStorageAccessor::make_secured_key(64);
    encoded_password_key = SecretStorageAccessor::encode_string(password_key);
    if (!SecretStorageAccessor::ensure_secret(email_key, "Email address")
        || !SecretStorageAccessor::ensure_secret(password_key, "Email password")) {
      std::println("failed to store secret, is the server running?");
      return -EREMOTEIO;
    }
    SecretStorageAccessor::release_secured_string(email_key);
    SecretStorageAccessor::release_secured_string(password_key);
  }
#endif

  // get a list of devices
  auto devices = NVMLSessionManager::get_manager().get_device_informations();
  if (devices.size() < config.gpu_count) {
    std::println(
      stderr, "requesting {} devices but only {} available on this system", config.gpu_count, devices.size()
    );
    return -ENOMEM;
  }

  std::vector<device_information> available_devices;
  available_devices = get_available_devices(config, devices);
  if (available_devices.size() < config.gpu_count) {
    if (config.wait_memory_timeout == 0) {
      std::println(stderr, "Insufficient device memory");
      return -ENOMEM;
    }
    // wait for free memory
    time_t                         start_wait_time = time(nullptr);
    std::unordered_map<pid_t, int> registered_pids;
    int                            epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
      perror("failed to open epoll");
      return -EIO;
    }
    while (true) {
      // add newly launched processes into account
      //  why they can launch but we cannot!? paruparu....
      for (const auto &device : devices) {
        auto processes = device.get_processes();
        for (const auto &process : processes) {
          if (!registered_pids.contains(static_cast<pid_t>(process.pid))) {
            int pid_fd = static_cast<int>(syscall(SYS_pidfd_open, process.pid, 0));
            registered_pids.insert({static_cast<pid_t>(process.pid), pid_fd});
            epoll_event event = {
              .events = EPOLLIN,
              .data   = {.u64 = (static_cast<uint64_t>(process.pid) << 32) | static_cast<uint64_t>(pid_fd)}
            };
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, pid_fd, &event);
          }
        }
      }

      // wait by epoll
      std::array<epoll_event, 8> events;
      int                        timeout      = 0;
      int                        current_time = static_cast<int>(time(nullptr));
      if (config.wait_memory_timeout != 0 && current_time - start_wait_time < config.wait_memory_timeout) {
        timeout = static_cast<int>(config.wait_memory_timeout - (current_time - start_wait_time)) * 1000;
      } else if (config.wait_memory_timeout == 0) {
        timeout = -1;
      }
      int n = epoll_wait(epoll_fd, events.data(), events.size(), timeout);
      if (n == 0) {
        std::println(stderr, "Timedout, insufficient memory.");
        return -ENOMEM;
      }
      for (int i = 0; i < n; i++) {
        auto pid = static_cast<pid_t>(events[i].data.u64 >> 32);
        auto fd  = static_cast<int>(events[i].data.u64 & 0xffffffffu);
        registered_pids.erase(pid);
        close(fd);
      }

      // check if we have enough memory now
      available_devices = get_available_devices(config, devices);
      if (available_devices.size() >= config.gpu_count) {
        for (const auto [pid, pid_fd] : registered_pids) {
          close(pid_fd);
        }
        close(epoll_fd);
        break;
      }
    }
  }

  std::string devices_to_use;
  size_t      start = 0;
  if (config.policy == Options::SelectionPolicy::BestFit) {
    start = available_devices.size() - config.gpu_count;
  } else if (config.policy == Options::SelectionPolicy::WorstFit) {
    start = 0;
  }
  std::vector<device_information> running_devices;
  running_devices.reserve(config.gpu_count);
  std::move(
    std::next(available_devices.begin(), static_cast<long>(start)),
    std::next(available_devices.begin(), static_cast<long>(start + config.gpu_count)),
    std::back_insert_iterator(running_devices)
  );
  for (const auto &device : running_devices) {
    if (!devices_to_use.empty()) {
      devices_to_use += ',';
    }
    devices_to_use += std::to_string(device.id);
  }
  std::println(stderr, "running on GPU: {}", devices_to_use);
  if (config.background) {
    pid_t pid = fork();
    if (pid == -1) {
      perror("failed to fork");
      return -errno;
    }
    if (pid != 0) {
      fprintf(stderr, "running in background with pid %d\n", pid);
      return 0;
    }
    // remap file descriptors
    int dev_null_fd = open("/dev/null", O_RDWR);
    if (dev_null_fd == -1) {
      perror("failed to open /dev/null");
    }
    fclose(stdin);
    fclose(stdout);
    fclose(stderr);
    if (dev_null_fd != -1) {
      dup2(dev_null_fd, STDIN_FILENO);
      dup2(dev_null_fd, STDOUT_FILENO);
      dup2(dev_null_fd, STDERR_FILENO);
      close(dev_null_fd);
      stderr = fdopen(STDERR_FILENO, "w");
      stdout = fdopen(STDOUT_FILENO, "w");
      setbuf(stderr, nullptr);
      setbuf(stdout, nullptr);
    }
  }
  setenv("CUDA_VISIBLE_DEVICES", devices_to_use.c_str(), 1);
  std::vector<std::unique_ptr<EnvironmentArranger>> environment_arrangers;
  if (config.monitor_gpu_memory) {
    // configure hooked API for tracing memory usage
    environment_arrangers.emplace_back(std::make_unique<MemoryWatcherEnvironmentArranger>(
      config.monitor_gpu_memory, std::move(running_devices)
    ));
  }
  if (!config.logging_path.empty()) {
    environment_arrangers.emplace_back(std::make_unique<LogEnvironmentArranger>(config.logging_path));
  }

  // at this point, the running_devices vector shall no longer be accessed
  //  since the ownership has been transferred

  // forking and/or executing
  //  first, launch all before-fork hooks
  for (const auto &arranger : environment_arrangers) {
    arranger->before_fork();
  }
  //  then, fork if we need to do that
  pid_t pid = config.direct_exec() ? 0 : fork();
  if (pid == -1) {
    perror("cannot fork");
    return -errno;
  }
  //   launch all after-fork hooks
  for (const auto &arranger : environment_arrangers) {
    arranger->after_fork(pid);
  }
  if (pid == 0) {
    if (!config.background) {
      // make the program executed foreground
      //  if we are launching an interactive program, keeping it background will cause it being paused
      //  when it tries to read from stdin
      // we may want find a better place to make this call, likely with the main block handling background
      //  executing, which need to be cleaned too
      tcsetpgrp(0, getpid());
    }
    // execute the program
    return do_launch(argv, config);
  }
  timespec start_time;
  clock_gettime(CLOCK_MONOTONIC, &start_time);
  int status;
  waitpid(pid, std::addressof(status), 0);

#ifdef HAVE_SECRET_STORAGE
  subprocess email_sender = {.pid = -1, .standard_input = nullptr};
  if (config.email_port != 0) {
    std::vector<std::string> arguments = {
      "send-email.py",
      "--one-time-key",
      "--email-key",
      std::string(encoded_email_key),
      "--password-key",
      std::string(encoded_password_key),
      "--server-address",
      config.email_server,
      "--server-port",
      std::to_string(config.email_port),
      "--subject",
      "Notification on task termination from glaunch",
    };
    if (!config.logging_path.empty()) {
      arguments.emplace_back("--attachment");
      arguments.emplace_back(config.logging_path);
    }
    email_sender = launch_subprocess(arguments);
    SecretStorageAccessor::release_secured_string(encoded_email_key);
    SecretStorageAccessor::release_secured_string(encoded_password_key);
    if (email_sender.standard_input == nullptr) {
      std::println(stderr, "failed to send email notification");
    } else {
      std::array<char, 100> hostname;
      gethostname(hostname.data(), hostname.size());
      std::println(
        email_sender.standard_input, "The process launched by glaunch on {} has terminated.", hostname.data()
      );
      if (!config.logging_path.empty()) {
        std::println(
          email_sender.standard_input, "The log file has been attached as attachment.", hostname.data()
        );
      }
      std::println(
        email_sender.standard_input,
        "The command line used to launch the process is {}.",
        std::ranges::subrange(std::next(args.cbegin(), static_cast<long>(config.break_point)), args.cend())
      );
    }
  }
#endif

  int return_value = 0;
  if (WIFEXITED(status)) {
    std::println(stderr, "program exited with code {}", WEXITSTATUS(status));
#ifdef HAVE_SECRET_STORAGE
    if (email_sender.standard_input != nullptr) {
      std::println(email_sender.standard_input, "program exited with code {}", WEXITSTATUS(status));
    }
#endif
    return_value = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    std::println(stderr, "program killed with signal {}", WTERMSIG(status));
#ifdef HAVE_SECRET_STORAGE
    if (email_sender.standard_input != nullptr) {
      std::println(email_sender.standard_input, "program killed with signal {}", WTERMSIG(status));
    }
#endif
    return_value = -EINTR;
  } else {
    std::println(stderr, "program terminated, but how?");
#ifdef HAVE_SECRET_STORAGE
    if (email_sender.standard_input != nullptr) {
      std::println(email_sender.standard_input, "program terminated, but how?");
    }
#endif
    return_value = -EAGAIN;
  }
  if (config.timing) {
    timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);
    unsigned long long total_time  = current_time.tv_sec - start_time.tv_sec;
    const auto         time_string = get_readable_duration(total_time);
    std::println(stderr, "elapsed time: {}", time_string);
#ifdef HAVE_SECRET_STORAGE
    if (email_sender.standard_input != nullptr) {
      std::println(email_sender.standard_input, "The process took {} to finish its run.", time_string);
    }
#endif
  }
#ifdef HAVE_SECRET_STORAGE
  if (email_sender.standard_input != nullptr) {
    fclose(email_sender.standard_input);
    waitpid(email_sender.pid, nullptr, 0);
  }
#endif
  return return_value;
}
