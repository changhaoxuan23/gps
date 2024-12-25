// glaunch-modules-memory-watcher - Monitor GPU memory usage for glaunch
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

#include <hooked_api_config.hh>
#include <memory-watcher.hh>

#include <ctime>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace GPS {
MemoryWatcher::parsed_trace_line::parsed_trace_line(std::string_view line) {
  auto name_split     = line.find_first_of('(');
  this->api_name      = line.substr(0, name_split);
  auto argument_split = line.find(") -> ");
  this->arguments     = line.substr(name_split + 1, argument_split - name_split - 1);
  this->succeed       = line[argument_split + 5] == '0';
}
auto MemoryWatcher::get_time() -> std::string {
  time_t     current_time = time(nullptr);
  struct tm *current_tm;
  current_tm = localtime(&current_time);
  std::array<char, 512> buffer;
  strftime(buffer.data(), buffer.size(), "[%EY %B %d %T]", current_tm);
  return {buffer.data()};
}

void MemoryWatcher::nvml_memory_watcher() {
  unsigned long long total_memory = 0;
  for (const auto &device : NVMLSessionManager::get_manager().get_device_informations()) {
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

void MemoryWatcher::trace_memory_watcher() {
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
        target     = strtoull(trace.arguments.substr(trace.arguments.find("ptr=[") + 5).data(), nullptr, 16);
        auto width = strtoull(trace.arguments.substr(trace.arguments.find("pitch=") + 6).data(), nullptr, 10);
        auto depth = strtoull(trace.arguments.substr(trace.arguments.find("depth=") + 6).data(), nullptr, 10);
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
MemoryWatcher::MemoryWatcher(MemoryMonitoringMethod method) : method(method) {
  this->pipes[0] = -1;
  this->pipes[1] = -1;
  this->pid      = -1;
}
auto MemoryWatcher::name() -> std::string_view { return "memory-watcher"; }
void MemoryWatcher::before_fork() {
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

auto MemoryWatcher::get_module_attribute() -> ModuleAttribute { return ModuleAttribute::KeepAlive; }

void MemoryWatcher::arrange_children() {
  if (this->pipes[0] != -1) {
    close(this->pipes[0]);
  }
}
void MemoryWatcher::arrange_parent(pid_t pid) {
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

template <>
auto prepare_module<MemoryWatcher>(Configurations &parser)
  -> std::function<std::unique_ptr<GLaunchModule>(
    const std::unordered_map<std::string, std::any> &arguments,
    const std::vector<std::string_view>             &raw_commandline
  )> {
  parser.add_option("--watch-memory", Configurations::CommonParsers::identity_parser, 1);
  return [](const std::unordered_map<std::string, std::any> &arguments, const std::vector<std::string_view> &)
           -> std::unique_ptr<GLaunchModule> {
    if (!arguments.contains("watch-memory")) {
      return {nullptr};
    }

    const auto            &list_ = std::any_cast<std::string>(arguments.at("watch-memory"));
    std::string_view       list{list_};
    size_t                 start = 0;
    MemoryMonitoringMethod method{MemoryMonitoringMethod::NONE};
    while (true) {
      auto end   = list.find_first_of(',', start);
      auto entry = list.substr(start, end - start);
      if (entry == "all") {
        method = MemoryMonitoringMethod::ALL;
      } else if (entry.starts_with("nvml")) {
        method |= MemoryMonitoringMethod::NVML;
        if (entry.size() > 5 && entry[4] == ':') {
          std::string               temporary{entry.substr(5)};
          std::span<std::string, 1> helper{&temporary, 1};
          method.nvml_query_interval = std::any_cast<unsigned long long>(
            Configurations::CommonParsers::duration_parser(helper.begin(), helper.end())
          );
        } else {
          method.nvml_query_interval = 5;
        }
      } else if (entry == "trace") {
        method |= MemoryMonitoringMethod::Trace;
      } else {
        std::println(stderr, "invalid type supplied to watch-memory: `{}'", entry);
        ::exit(EXIT_FAILURE);
      }
      if (end == entry.npos) {
        break;
      }
      start = end + 1;
    }

    return std::make_unique<MemoryWatcher>(method);
  };
}
template <> void show_help<MemoryWatcher>() {
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
}
} // namespace GPS