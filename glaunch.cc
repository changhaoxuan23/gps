// glaunch - Launch computational process on proper GPUs regards to memory
// availability Copyright (C) 2023-2024 Haoxuan
// Chang<changhaoxuan23@mails.ucas.ac.cn>

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

#include "nvml_common.hh"
#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <chrono>
#include <configuration.hh>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <functional>
#include <print>
#include <string>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#ifdef HAVE_SECRET_STORAGE
#include <secret_storage_accessor.hh>
#endif

struct Options {
  enum class SelectionPolicy { BestFit, WorstFit };
  bool            background;
  bool            timing;
  uint16_t        gpu_count;
  SelectionPolicy policy;
  uint32_t        wait_memory_timeout;
  std::string     logging_path;
  size_t          break_point;
  uint64_t        monitor_gpu_memory;
  size_t          memory_estimation;
#ifdef HAVE_SECRET_STORAGE
  std::string email_server;
  uint16_t    email_port;
#endif
  [[nodiscard]] auto direct_exec() const -> bool {
    return !this->timing && this->monitor_gpu_memory == 0
#ifdef HAVE_SECRET_STORAGE
        && this->email_port == 0
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
      this->monitor_gpu_memory = std::any_cast<unsigned long long>(parse_result.at("watch-memory"));
    } else {
      this->monitor_gpu_memory = 0;
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
    this->add_option("--watch-memory", CommonParsers::duration_parser, 1);
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
    std::println("  --watch-memory DURATION    Dump GPU memory usage every DURATION seconds               ");
    std::println("                              suffixes are supported, try m, h, d                       ");
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
static void
gpu_memory_watcher(const pid_t pid, const std::vector<device_information> &devices, const Options &config) {
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(config.monitor_gpu_memory));
    unsigned long long total_memory = 0;
    for (const auto &device : devices) {
      auto processes = device.get_processes();
      for (const auto &process : processes) {
        if (getpgid(static_cast<pid_t>(process.pid)) == pid) {
          total_memory += process.usedGpuMemory;
        }
      }
    }
    time_t     current_time = time(nullptr);
    struct tm *current_tm;
    current_tm = localtime(&current_time);
    std::array<char, 512> buffer;
    strftime(buffer.data(), buffer.size(), "[%EY %B %d %T]", current_tm);
    std::println(stderr, "{} {} GPU memory in use", buffer.data(), get_readable_size(total_memory).c_str());
  }
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

auto main(int argc, char *argv[]) -> int {
  std::println("glaunch v0.0.3 licensed under AGPLv3 or later");
  std::println("you can goto https://github.com/changhaoxuan23/gps for source code\n");

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
  if (!config.logging_path.empty()) {
    // setup logging first: we use tee to do this job, assuming which in
    // installed on the system
    //  since it is part of the GNU coreutils, it shall be safe to make such an
    //  assumption in common cases
    int pipes[2];
    if (pipe(pipes) == -1) {
      perror("failed to make pipe");
      return -errno;
    }
    pid_t pid = fork();
    if (pid == -1) {
      perror("cannot fork");
      return -errno;
    }
    if (pid == 0) {
      dup2(pipes[0], STDIN_FILENO);
      close(pipes[0]);
      close(pipes[1]);
      execlp("tee", "tee", config.logging_path.c_str(), nullptr);
      // you shall not be here
      perror("cannot exec tee");
      exit(-errno);
    }
    // close and reopen stdout/stderr on the pipe
    fclose(stdout);
    fclose(stderr);
    dup2(pipes[1], STDOUT_FILENO);
    dup2(pipes[1], STDERR_FILENO);
    close(pipes[0]);
    close(pipes[1]);
    stderr = fdopen(STDERR_FILENO, "w");
    stdout = fdopen(STDOUT_FILENO, "w");
    setbuf(stderr, nullptr);
    setbuf(stdout, nullptr);
  }
  if (config.direct_exec()) {
    return do_launch(argv, config);
  }
  pid_t pid = fork();
  if (pid == -1) {
    perror("cannot fork");
    return -errno;
  }
  if (pid == 0) {
    return do_launch(argv, config);
  }
  timespec start_time;
  clock_gettime(CLOCK_MONOTONIC, &start_time);
  if (config.monitor_gpu_memory != 0) {
    std::thread(gpu_memory_watcher, pid, std::ref(running_devices), std::ref(config)).detach();
  }
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