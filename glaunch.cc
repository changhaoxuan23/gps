// glaunch - Launch computational process on proper GPUs regards to memory availability
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

#include "nvml_common.hh"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <string>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>
#define EXEC_NAME "glaunch"
#define GLAUNCH_VERSION "v0.0.2"
struct Configurations {
private:
  using parser_argument_iterator_t = std::vector<std::string>::const_iterator;
  using parser_t = std::function<void(parser_argument_iterator_t, parser_argument_iterator_t)>;
  struct Option {
    const parser_t parser;
    std::string name;
    size_t argument_count;

    Option(parser_t parser, std::string name, size_t argument_count)
        : parser(std::move(parser)), name(std::move(name)), argument_count(argument_count) {}
  };
  template <typename T> static auto full_convert_ull(const std::string &value) -> T {
    size_t pos = 0;
    unsigned long long result;
    try {
      result = std::stoi(value, std::addressof(pos));
    } catch (const std::exception &error) {
      pos = 0;
    }
    if (value.size() == 0 || pos == 0) {
      fprintf(stderr, "cannot convert %s.\n", value.c_str());
      exit(EXIT_FAILURE);
    }
    return static_cast<T>(result);
  }
  static void true_saver(bool &target, parser_argument_iterator_t begin, parser_argument_iterator_t end) {
    assert(begin == end);
    target = true;
  }
  static void duration_saver(
      unsigned long long &target, parser_argument_iterator_t begin, parser_argument_iterator_t end
  ) {
    assert(std::next(begin) == end);
    size_t pos = 0;
    try {
      target = std::stoull(*begin, std::addressof(pos));
    } catch (const std::exception &e) {
      fprintf(stderr, "invalid value %s\n", begin->c_str());
      exit(EXIT_FAILURE);
    }
    if (pos != begin->size()) {
      auto suffix = begin->substr(pos);
      std::for_each(suffix.begin(), suffix.end(), [](auto &c) { c = tolower(c); });
      const auto &map = get_duration_suffix_map();
      auto iter = map.find(suffix);
      if (iter == map.cend()) {
        fprintf(stderr, "invalid suffix %s\n", begin->substr(pos).c_str());
        exit(EXIT_FAILURE);
      }
      target *= iter->second;
    }
  }
  static void
  parser_dispatcher(const Option &option, size_t &break_point, const std::vector<std::string> &args) {
    if (args[break_point] == option.name) {
      if (args.size() <= break_point + option.argument_count) {
        fprintf(stderr, "%s expects arguments but not provided\n", option.name.c_str());
        exit(EXIT_FAILURE);
      }
      auto step = static_cast<parser_argument_iterator_t::difference_type>(++break_point);
      option.parser(
          std::next(args.cbegin(), step),
          std::next(args.cbegin(), step + static_cast<decltype(step)>(option.argument_count))
      );
      break_point += option.argument_count;
    } else if (option.argument_count == 1 && args[break_point].substr(0, option.name.size() + 1) == (option.name + '=')) {
      std::vector<std::string> temporary;
      temporary.emplace_back(args[break_point++].substr(option.name.size() + 1));
      option.parser(temporary.cbegin(), temporary.cend());
    } else {
      fprintf(
          stderr, "invalid option string %s with option name %s\n", args[break_point].c_str(),
          option.name.c_str()
      );
      exit(EXIT_FAILURE);
    }
  }
  static auto test_option(const Option &option, size_t &break_point, const std::vector<std::string> &args)
      -> bool {
    if (args[break_point].substr(0, option.name.size()) == option.name) {
      parser_dispatcher(option, break_point, args);
      return true;
    }
    return false;
  }
  void parse_gpu_count(parser_argument_iterator_t begin, parser_argument_iterator_t end) {
    assert(std::next(begin) == end);
    if (this->gpu_count != 1) {
      fprintf(stderr, "multiple instance of option --gpus, the last one takes effect\n");
    }
    this->gpu_count = full_convert_ull<unsigned int>(*begin);
    if (this->gpu_count > 16) {
      fprintf(stderr, "%u GPUs? Amazing, you lucky guy!\n", this->gpu_count);
    }
  }
  void parse_memory_estimation(parser_argument_iterator_t begin, parser_argument_iterator_t end) {
    assert(std::next(begin) == end);
    if (this->memory_estimation != NoEstimation) {
      fprintf(
          stderr, "multiple instance of option --memory-budget, the last "
                  "one takes effect\n"
      );
    }
    size_t pos = 0;
    try {
      this->memory_estimation = std::stoull(*begin, std::addressof(pos));
    } catch (const std::exception &e) {
      fprintf(stderr, "invalid value %s\n", begin->c_str());
      exit(EXIT_FAILURE);
    }
    if (pos != begin->size()) {
      auto suffix = begin->substr(pos);
      std::for_each(suffix.begin(), suffix.end(), [](auto &c) { c = tolower(c); });
      const auto &map = get_size_suffix_map();
      auto iter = map.find(suffix);
      if (iter == map.cend()) {
        fprintf(stderr, "invalid suffix %s\n", begin->substr(pos).c_str());
        exit(EXIT_FAILURE);
      }
      this->memory_estimation *= iter->second;
    }
    if (this->memory_estimation > 0x2000000000ull) {
      fprintf(stderr, "%llu bytes! you must be doing something fascinating!\n", this->memory_estimation);
    }
  }
  void parse_policy(parser_argument_iterator_t begin, parser_argument_iterator_t end) {
    assert(std::next(begin) == end);
    std::string test(*begin);
    std::for_each(test.begin(), test.end(), [](auto &c) { c = tolower(c); });
    if (test.compare("worst") == 0 || test.compare("worstfit") == 0) {
      this->policy = SelectionPolicy::WorstFit;
    } else if (test.compare("best") == 0 || test.compare("bestfit") == 0) {
      this->policy = SelectionPolicy::BestFit;
    } else {
      fprintf(stderr, "invalid policy %s\n", begin->c_str());
      exit(EXIT_FAILURE);
    }
  }
  void parse_logging_path(parser_argument_iterator_t begin, parser_argument_iterator_t end) {
    assert(std::next(begin) == end);
    this->logging_path = *begin;
  }
  void parse_wait_memory_timeout(parser_argument_iterator_t begin, parser_argument_iterator_t end) {
    duration_saver(this->wait_memory_timeout, begin, end);
    if (this->wait_memory_interval == 0) {
      this->wait_memory_interval = 60;
    }
  }
  void parse_wait_memory_interval(parser_argument_iterator_t begin, parser_argument_iterator_t end) {
    duration_saver(this->wait_memory_interval, begin, end);
    if (this->wait_memory_timeout == 0) {
      this->wait_memory_timeout = 3600;
    }
  }
  static void help(parser_argument_iterator_t begin, parser_argument_iterator_t end) {
    assert(begin == end);
    printf("Launch computational process on proper GPUs regards to "
           "memory availability\n");
    printf("Usage: " EXEC_NAME " [OPTIONS...] [--] PROGRAM [ARGS...]\n");
    printf("OPTIONS:\n");
    printf("  --gpus GPU_COUNT              Use GPU_COUNT gpus for this program, defaults to 1\n\n");
    printf("  --memory-budget MEMORY_SIZE   Slight over-estimated size of memory your program will\n");
    printf("                                 consume per GPU. Suffixes are allowed to simplify this\n");
    printf("                                 configuration, try KiB, MiB, GiB, etc.. If you do not\n");
    printf("                                 specify such value, we assume that your program could\n");
    printf("                                 run with arbitrary amount of memory\n\n");
    printf("  --policy POLICY               Policy used to select GPU devices. Currently two policies\n");
    printf("                                 are supported while we defaults to the first one:\n");
    printf("                                  WorstFit: MAXIMIZE free space after your program launches\n");
    printf("                                  BestFit: MINIMIZE free space after your program launches\n\n");
    printf("  --time                        When the program terminates, summary its elapsed time\n\n");
    printf("  --log PATH                    Duplicate and save stdout and stderr to PATH\n\n");
    printf("  --watch-memory DURATION       Dump GPU memory usage every DURATION seconds, suffixes are\n");
    printf("                                 supported, try m, h, d\n\n");
    printf("  --wait-timeout DURATION       Wait for no more than DURATION if currently no device have\n");
    printf("                                 sufficient memory launching the specified process. Suffixes\n");
    printf("                                 are supported. If --wait-interval is specified while this is\n");
    printf("                                 not, defaults to 1h\n\n");
    printf("  --wait-interval DURATION      Check for memory availability for each DURATION seconds.\n");
    printf("                                 Suffixes are supported. If --wait-timeout is specified while\n");
    printf("                                 this is not, defaults to 1m\n\n");
    printf("  --help                        Show this message again\n");
    printf("\n");
    printf("If you got some trouble on argument parsing, which may be triggered by a program whose name\n");
    printf(R"( starts with '--', you can add '--' before it to terminate option parsing manually)");
    printf("\n\n");
    printf("PROGRAM: the program to launch\n");
    printf("ARGS: arguments passed to PROGRAM which will not be modified\n");
    exit(EXIT_SUCCESS);
  }

public:
  static constexpr unsigned long long NoEstimation = -1;
  enum class SelectionPolicy {
    BestFit,  // minimize difference between the free memory on the GPU and your
              // budget
    WorstFit, // maximize difference between the free memory on the GPU and your
              // budget
  };

  // index of the first command line component after options
  size_t break_point;

  // number of GPUs to use
  unsigned int gpu_count{1};

  // slightly overly estimated memory budget per GPU
  unsigned long long memory_estimation{NoEstimation};

  // policy when selecting GPU
  SelectionPolicy policy{SelectionPolicy::WorstFit};

  // if we shall measure (elapsed) time of the program
  bool timing{false};

  // destination path to duplicate and store output from the program
  std::string logging_path{};

  // time interval between two samples on GPU memory consumption are taken
  unsigned long long monitor_gpu_memory{0};

  // wait-for-free-memory
  unsigned long long wait_memory_timeout{0};
  unsigned long long wait_memory_interval{0};

  Configurations(const std::vector<std::string> &args) {
    this->break_point = 1;
    std::vector<Option> options;
    options.emplace_back(
        [this](parser_argument_iterator_t begin, parser_argument_iterator_t end) {
          this->parse_gpu_count(begin, end);
        },
        "--gpus", 1
    );
    options.emplace_back(
        [this](parser_argument_iterator_t begin, parser_argument_iterator_t end) {
          this->parse_memory_estimation(begin, end);
        },
        "--memory-budget", 1
    );
    options.emplace_back(
        [this](parser_argument_iterator_t begin, parser_argument_iterator_t end) {
          this->parse_policy(begin, end);
        },
        "--policy", 1
    );
    options.emplace_back(
        [](parser_argument_iterator_t begin, parser_argument_iterator_t end) {
          Configurations::help(begin, end);
        },
        "--help", 0
    );
    options.emplace_back(
        [this](parser_argument_iterator_t begin, parser_argument_iterator_t end) {
          Configurations::true_saver(this->timing, begin, end);
        },
        "--time", 0
    );
    options.emplace_back(
        [this](parser_argument_iterator_t begin, parser_argument_iterator_t end) {
          this->parse_logging_path(begin, end);
        },
        "--log", 1
    );
    options.emplace_back(
        [this](parser_argument_iterator_t begin, parser_argument_iterator_t end) {
          duration_saver(this->monitor_gpu_memory, begin, end);
        },
        "--watch-memory", 1
    );
    options.emplace_back(
        [this](parser_argument_iterator_t begin, parser_argument_iterator_t end) {
          this->parse_wait_memory_timeout(begin, end);
        },
        "--wait-timeout", 1
    );
    options.emplace_back(
        [this](parser_argument_iterator_t begin, parser_argument_iterator_t end) {
          this->parse_wait_memory_interval(begin, end);
        },
        "--wait-interval", 1
    );

    while (this->break_point < args.size() && args[this->break_point].substr(0, 2).compare("--") == 0) {
      if (args[this->break_point].size() == 2) {
        // it is just '--'
        ++this->break_point;
        break;
      }
      bool matched = false;
      for (const auto &option : options) {
        if (test_option(option, this->break_point, args)) {
          matched = true;
          break;
        }
      }
      if (!matched) {
        fprintf(stderr, "unrecognized option %s\n", args[this->break_point].c_str());
        exit(EXIT_FAILURE);
      }
    }
  }

  void dump(FILE *target) const {
    fprintf(target, "========== configuration dump ==========\n");
    fprintf(target, "  gpu_count: %u\n", this->gpu_count);
    fprintf(target, "  memory_estimation: %llu\n", this->memory_estimation);
    fprintf(target, "  policy: %s\n", this->policy == SelectionPolicy::BestFit ? "BestFit" : "WorstFit");
    fprintf(target, "  timing: %s\n", this->timing ? "true" : "false");
    fprintf(target, "  logging_path: %s\n", this->logging_path.c_str());
    fprintf(target, "  monitor_gpu_memory: %llu\n", this->monitor_gpu_memory);
    fprintf(target, "========== configuration dump ==========\n");
  }

  [[nodiscard]] auto direct_exec() const -> bool { return !this->timing && this->monitor_gpu_memory == 0; }
};
// launch the actual process: this function will run in the process which will, by this function, execvp(2)
//  into the actual process
// if we have forked off can be checked via !config.direct_exec()
static auto do_launch(char *argv[], const Configurations &config) -> int {
  // set process group id to group processes forked from the actual computing process
  setpgid(0, 0);
  if (!config.direct_exec()) { // arrange tasks that shall be done only if we are forking off here
    // make this process get killed when the controlling glaunch process is killed
    // note that this takes no effect if the program to be executed has set-user-id/set-group-id or
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
static void gpu_memory_watcher(
    const pid_t pid, const std::vector<unsigned int> &device_ids, const Configurations &config
) {
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(config.monitor_gpu_memory));
    unsigned long long total_memory = 0;
    for (const auto id : device_ids) {
      nvmlDevice_t device = nullptr;
      nvmlReturn_t return_value = nvmlDeviceGetHandleByIndex(id, &device);
      if (return_value != NVML_SUCCESS) {
        fprintf(stderr, "failed to open device %u: %s, skipping.\n", id, nvmlErrorString(return_value));
        continue;
      }
      auto [status, processes] = get_processes_on_device(device);
      if (status != NVML_SUCCESS) {
        fprintf(
            stderr, "failed to get processes on device %u: %s, skipping.\n", id, nvmlErrorString(return_value)
        );
        continue;
      }
      for (const auto &process : processes) {
        if (getpgid(static_cast<pid_t>(process.pid)) == pid) {
          total_memory += process.usedGpuMemory;
        }
      }
    }
    time_t current_time = time(nullptr);
    struct tm *current_tm;
    current_tm = localtime(&current_time);
    char buffer[512];
    strftime(buffer, sizeof(buffer), "[%EY %B %d %T]", current_tm);
    fprintf(stderr, "%s %s GPU memory in use\n", buffer, get_readable_size(total_memory).c_str());
  }
}
// get devices with sufficient memory, sort in decreasing order of free memory
static auto get_available_devices(const Configurations &config) -> std::vector<device_information> {
  unsigned int device_count = 0;
  panic_on_failure(nvmlDeviceGetCount, &device_count);
  std::vector<device_information> devices_;
  devices_.reserve(device_count);
  for (unsigned int i = 0; i < device_count; i++) {
    nvmlDevice_t device = nullptr;
    nvmlReturn_t return_value = nvmlDeviceGetHandleByIndex(i, &device);
    if (return_value != NVML_SUCCESS) {
      fprintf(stderr, "failed to open device %u: %s, skipping.\n", i, nvmlErrorString(return_value));
      continue;
    }
    devices_.emplace_back(device);
  }
  std::sort(
      devices_.begin(), devices_.end(),
      [](const device_information &lhs, const device_information &rhs) -> bool {
        return lhs.memory.free > rhs.memory.free;
      }
  );
#ifndef NDEBUG
  for (const auto &device : devices_) {
    fprintf(
        stderr, "%u (%s): %llu / %llu\n", device.id, device.name.c_str(), device.memory.free,
        device.memory.total
    );
  }
#endif
  std::vector<device_information> devices;
  std::copy_if(
      devices_.cbegin(), devices_.cend(), std::back_inserter(devices),
      [config](const device_information &device) -> bool {
        return config.memory_estimation == Configurations::NoEstimation
                   ? true
                   : config.memory_estimation < device.memory.free;
      }
  );
  return devices;
}
auto main(int argc, char *argv[]) -> int {
  fprintf(stdout, EXEC_NAME " " GLAUNCH_VERSION " licensed under AGPLv3 or later\n");
  fprintf(stdout, "you can goto https://github.com/changhaoxuan23/gps for source code\n\n");
  std::vector<std::string> args;
  args.reserve(argc);
  for (int i = 0; i < argc; i++) {
    args.emplace_back(argv[i]);
  }
  Configurations config(args);
  config.dump(stdout);

  panic_on_failure(nvmlInit);

  // wait for free memory
  time_t start_wait_time = time(nullptr);
  std::vector<device_information> available_devices;
  while (true) {
    available_devices = get_available_devices(config);
    if (available_devices.size() >= config.gpu_count) {
      break;
    }
    time_t current_time = time(nullptr);
    if (static_cast<unsigned long long>(current_time - start_wait_time) >= config.wait_memory_timeout) {
      fprintf(
          stderr, "not enough devices with sufficient memory that satisfy "
                  "your request\n"
      );
      return -ENOMEM;
    }
    unsigned int time_to_timeout = config.wait_memory_timeout - (current_time - start_wait_time);
    unsigned int time_to_sleep =
        time_to_timeout < config.wait_memory_interval ? time_to_timeout : config.wait_memory_interval;
    while (time_to_sleep > 0) {
      time_to_sleep = sleep(time_to_sleep);
    }
  }
  std::string devices_to_use;
  unsigned int count = 0;
  size_t start = 0;
  if (config.policy == Configurations::SelectionPolicy::BestFit) {
    start = available_devices.size() - config.gpu_count;
  } else if (config.policy == Configurations::SelectionPolicy::WorstFit) {
    start = 0;
  }
  fprintf(stderr, "running on GPU: ");
  std::vector<unsigned int> device_ids;
  device_ids.reserve(config.gpu_count);
  for (size_t i = start; count < config.gpu_count; i++, count++) {
    if (!devices_to_use.empty()) {
      devices_to_use += ',';
      fprintf(stderr, ", ");
    }
    devices_to_use += std::to_string(available_devices.at(i).id);
    fprintf(stderr, "%u", available_devices.at(i).id);
    device_ids.emplace_back(available_devices.at(i).id);
  }
  fprintf(stderr, "\n");
  setenv("CUDA_VISIBLE_DEVICES", devices_to_use.c_str(), 1);
  if (!config.logging_path.empty()) {
    // setup logging first: we use tee to do this job, assuming which in installed on the system
    //  since it is part of the GNU coreutils, it shall be safe to make such an assumption in common cases
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
    std::thread(gpu_memory_watcher, pid, std::ref(device_ids), std::ref(config)).detach();
  }
  int status;
  waitpid(pid, std::addressof(status), 0);
  int return_value = 0;
  if (WIFEXITED(status)) {
    fprintf(stderr, "program exited with code %d\n", WEXITSTATUS(status));
    return_value = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    fprintf(stderr, "program killed with signal %d\n", WTERMSIG(status));
    return_value = -EINTR;
  } else {
    fprintf(stderr, "program terminated, but how?\n");
    return_value = -EAGAIN;
  }
  if (config.timing) {
    timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);
    unsigned long long total_time = current_time.tv_sec - start_time.tv_sec;
    fprintf(stderr, "elapsed time: %s\n", get_readable_duration(total_time).c_str());
  }
  return return_value;
}