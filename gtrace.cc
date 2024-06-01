// gtrace - Trace GPU API (CUDA API) calls like strace for system calls
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

#include <configuration.hh>
#include <hooked_api_config.hh>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <print>
#include <unistd.h>
#include <vector>

// commandline argument parser
struct Options {
  std::string filter;
  std::string output;
  bool        include_backtrace{false};
  size_t      break_point;

  Options(const std::unordered_map<std::string, std::any> &parse_result) {
    if (parse_result.contains("backtrace") || parse_result.contains("stacktrace")
        || parse_result.contains("calltrace")) {
      this->include_backtrace = true;
    }
    if (parse_result.contains("output")) {
      this->output = std::any_cast<std::string>(parse_result.at("output"));
    }
    if (parse_result.contains("filter-regex")) {
      this->filter = std::any_cast<std::string>(parse_result.at("filter-regex"));
    }

    this->break_point = std::any_cast<size_t>(parse_result.at("_break_point"));
  }
};
class Parser : public Configurations {
public:
  Parser() {
    this->add_option("--backtrace", CommonParsers::true_parser, 0);
    this->add_option("--stacktrace", CommonParsers::true_parser, 0);
    this->add_option("--calltrace", CommonParsers::true_parser, 0);

    this->add_option("--filter-regex", CommonParsers::identity_parser, 1);

    this->add_option("--output", CommonParsers::identity_parser, 1);
  }
  void help() const override {
    std::println("gtrace: Trace CUDA API calls made by certain process, just like strace to syscall.       ");
    std::println("  Usage: gtrace [OPTIONS...] [--] PROGRAM [ARGS...]                                      ");
    std::println("  OPTIONS:                                                                               ");
    std::println("    --backtrace    Include callstack alongside with each API call traced. gtrace is aware");
    std::println("    --stacktrace    of both the physical stack, the one familiar to C users, and Python  ");
    std::println("    --calltrace     stack, which provides more readable and meaningful symbols if a call ");
    std::println("                    is made from Python code.                                            ");
    std::println("                                                                                         ");
    std::println("    --filter-regex Specify filter on APIs to be reported. CUDA APIs will only be reported");
    std::println("                    if whose full name can be matched exactly by the regular expression. ");
    std::println("                    The string is matched case-sensitively.                              ");
    std::println("                                                                                         ");
    std::println("    --output       Write output to this path instead of to stderr.                       ");
    std::println("                                                                                         ");
    std::println("    --help         Show this message again                                               ");
    std::println("                                                                                         ");
    std::println("                                                                                         ");
    std::println("  PROGRAM: the program to launch                                                         ");
    std::println("  ARGS: arguments passed to PROGRAM which will not be modified                           ");
    std::println("                                                                                         ");
    std::println("  Notes:                                                                                 ");
    std::println("    If you got some trouble on argument parsing, which may be triggered by a program     ");
    std::println("     whose name starts with '--', you can add '--' before it to terminate option parsing.");
    std::println("     for example, gtrace --backtrace -- --your-program                                   ");
  }
};

auto main(int argc, char **argv) -> int {
  std::println("gtrace in gps build v{}, licensed under AGPLv3 or later", GPS_VERSION);
  std::println("you can goto https://github.com/changhaoxuan23/gps for source code\n");
  if (argc == 1) {
    std::println(stderr, "invalid usage, invoke `gtrace --help' to see how to use gtrace.");
    exit(EXIT_FAILURE);
  }
  Options                   commandline(Parser().parse({argv, argv + argc}));
  GPSHookedAPIConfiguration config;
  auto                     &sink = config.outputs.emplace_back();
  if (commandline.output.empty()) {
    sink.output_fd = STDERR_FILENO;
  } else {
    sink.output_fd = open(commandline.output.c_str(), O_WRONLY | O_TRUNC | O_CREAT, 0600);
    if (sink.output_fd == -1) {
      std::println(
        stderr,
        "failed to open {} for writing: {}. falling back to stderr",
        commandline.output,
        strerror(errno)
      );
      sink.output_fd = STDERR_FILENO;
    }
  }
  sink.include_backtrace = commandline.include_backtrace;
  sink.filter            = commandline.filter;
  setup_api_hook(std::move(config));
  std::vector<char *> arguments{argv + commandline.break_point, argv + argc};
  arguments.emplace_back(nullptr);
  execvp(arguments[0], arguments.data());
  std::println(stderr, "failed to launch real process: {}", strerror(errno));
  exit(EXIT_FAILURE);
}