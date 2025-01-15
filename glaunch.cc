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
#include <functional>
#include <glaunch-modules/email-notifier.hh>
#include <glaunch-modules/glaunch-module.hh>
#include <glaunch-modules/logging.hh>
#include <glaunch-modules/memory-watcher.hh>
#include <glaunch-modules/timing.hh>
#include <hooked_api_config.hh>
#include <nvml_common.hh>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <forward_list>
#include <print>
#include <string>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

struct Options {
  enum class SelectionPolicy : uint8_t { BestFit, WorstFit };

  // the program to be launched shall be started in background
  //  to be specific, it shall be forked and detached from the current controlling shell with output closed
  bool            background;
  uint16_t        gpu_count;
  SelectionPolicy policy;
  uint32_t        wait_memory_timeout;
  size_t          break_point;
  size_t          memory_estimation;

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

    this->background = parse_result.contains("background");

    if (parse_result.contains("wait-timeout")) {
      this->wait_memory_timeout = std::any_cast<unsigned long long>(parse_result.at("wait-timeout"));
    } else {
      this->wait_memory_timeout = 0;
    }

    this->break_point = std::any_cast<size_t>(parse_result.at("_break_point"));
  }
  void dump(FILE *file) const {
    std::println(file, "------ core options dump ------");
    std::println(file, "background   : {}", this->background ? "true" : "false");
    std::println(file, "break_point  : {}", this->break_point);
    std::println(file, "gpu_count    : {}", this->gpu_count);
    std::println(file, "memory_budget: {}", this->memory_estimation);
    std::println(file, "policy       : {}Fit", this->policy == SelectionPolicy::BestFit ? "Best" : "Worst");
    std::println(file, "wait_timeout : {}", this->wait_memory_timeout);
    std::println(file, "------ core options dump ------");
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
    this->add_option("--background", CommonParsers::true_parser, 0);
    this->add_option("--wait-timeout", CommonParsers::duration_parser, 1);
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
    GPS::show_help<GPS::Timing>();
    GPS::show_help<GPS::Logging>();
    GPS::show_help<GPS::MemoryWatcher>();
    GPS::show_help<GPS::EmailNotifier>();
    std::println("  --background               Return after the process is launched, effectively run it   ");
    std::println("                              in background. This will also remap file descriptors so   ");
    std::println("                              that all output from the program will be discarded.       ");
    std::println("                             A program launched in background mode will keep running    ");
    std::println("                              even after you closed the terminal and logged out.        ");
    std::println("                             See also --log.                                            ");
    std::println("                                                                                        ");
    std::println("  --wait-timeout DURATION    Wait for no more than DURATION if no device have sufficient");
    std::println("                              memory launching the specified process, -1 for infinity.  ");
    std::println("                             Suffixes are supported.                                    ");
    std::println("                                                                                        ");
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

// launch the actual process: this function will run in the
// process which will, by this function, execvp(2) into the actual process
static auto do_launch(char *argv[], const Options &config) -> int {
  // set process group id to group processes forked from the actual computing process
  setpgid(0, 0);

  if (!config.background) {
    // make the program executed foreground
    //  if we are launching an interactive program, keeping it background will cause it being paused
    //  when it tries to read from stdin
    tcsetpgrp(0, getpid());
  }

  // make this process get killed when the controlling glaunch process is killed
  // note that this takes no effect if the program to be executed has set-user-id/set-group-id
  // or capabilities. If not sure, see prctl(2)
  prctl(PR_SET_PDEATHSIG, SIGKILL);

  // logging the command line to be executed
  std::print(stderr, "executing: [");
  for (auto i = config.break_point; argv[i] != nullptr; i++) {
    if (i != config.break_point) {
      std::print(stderr, ", ");
    }
    fputc('\'', stderr);
    for (auto c = argv[i]; *c != '\0'; c++) {
      if (*c == '\'') {
        std::print(stderr, R"('"'"')");
      } else {
        fputc(*c, stderr);
      }
    }
    fputc('\'', stderr);
  }
  std::println(stderr, "]...");

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

static auto get_modules_list() -> std::forward_list<std::unique_ptr<GPS::GLaunchModule>> & {
  static std::forward_list<std::unique_ptr<GPS::GLaunchModule>> list;
  return list;
}
auto GPS::get_all_modules() -> const std::forward_list<std::unique_ptr<GPS::GLaunchModule>> & {
  return get_modules_list();
}

auto main(int argc, char *argv[]) -> int {
  std::println("glaunch in gps build v{}, licensed under AGPLv3 or later", GPS_VERSION);
  std::println("you can goto https://github.com/changhaoxuan23/gps for source code\n");
  if (argc == 1) {
    std::println(stderr, "invalid usage, invoke `glaunch --help' to see how to use glaunch.");
    exit(EXIT_FAILURE);
  }

  // prepare modules
  Parser parser;
  std::forward_list<std::function<std::unique_ptr<
    GPS::
      GLaunchModule>(const std::unordered_map<std::string, std::any> &, const std::vector<std::string_view> &)>>
    commandline_callbacks;
  commandline_callbacks.emplace_front(GPS::prepare_module<GPS::Logging>(parser));
  commandline_callbacks.emplace_front(GPS::prepare_module<GPS::MemoryWatcher>(parser));
  commandline_callbacks.emplace_front(GPS::prepare_module<GPS::Timing>(parser));

  std::vector<std::string> args(argv, argv + argc);
  auto                     parsed_commandline = parser.parse(args);
  Options                  config(parsed_commandline);
  auto                    &modules = get_modules_list();
  for (const auto &callback : commandline_callbacks) {
    auto module = callback(parsed_commandline, {argv, argv + argc});
    if (module) {
      modules.emplace_front(std::move(module));
    }
  }
  config.dump(stdout);

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
      std::println(stderr, "running in background with pid {}", pid);
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
      setvbuf(stderr, nullptr, _IONBF, 0);
      setvbuf(stdout, nullptr, _IONBF, 0);
    }
  }
  setenv("CUDA_VISIBLE_DEVICES", devices_to_use.c_str(), 1);

  // forking and/or executing
  //  first, launch all before-fork hooks
  for (const auto &module : modules) {
    module->before_fork();
  }
  //  then, fork if we need to do that
  pid_t pid = std::ranges::any_of(
                modules,
                [](const auto &module) -> bool {
                  return module->get_module_attribute() & GPS::GLaunchModule::ModuleAttribute::KeepAlive;
                }
              )
              ? fork()
              : 0;
  if (pid == -1) {
    perror("cannot fork");
    return -errno;
  }
  //   launch all before-exec hooks
  for (const auto &module : modules) {
    module->before_exec(pid);
  }
  if (pid == 0) {
    // execute the program
    return do_launch(argv, config);
  }

  // wait for the program launched to terminate
  int status_;
  waitpid(pid, std::addressof(status_), 0);

  // construct structured exit status
  GPS::ExitStatus status(status_);

  // launch all hooks after the program terminates
  for (const auto &module : modules) {
    module->after_program_exit(status);
  }

  int return_value = 0;
  if (status.exited_normally) {
    std::println(stderr, "program exited with code {}", status.exit_code);
    return_value = status.exit_code;
  } else if (status.killed_by_signal) {
    std::println(stderr, "program killed with signal {}", status.signal_id);
    return_value = -EINTR;
  } else {
    std::println(stderr, "program terminated, but how?");
    return_value = -EAGAIN;
  }
  return return_value;
}
