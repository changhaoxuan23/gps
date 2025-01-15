// glaunch-modules-logging - Handle logging (output redirection) for glaunch
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

#include <logging.hh>

#if __GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 34)
#define USE_FALLBACK_CLOSE_RANGE
#endif

#ifdef USE_FALLBACK_CLOSE_RANGE
#include <cerrno>
#endif

namespace {
// since it can be common for servers to have glibc not recent enough to provide close_range(2)
//  we try to create an stub version of which that utilizes syscall(2) directly
//  as per manpage, close_range(2) is available since kernel 5.9 and is provided by glibc since
//  version 2.34: we use ::syscall(436, ...) directly to make such syscall if glibc is not recent
//  enough. If the kernel is also not recent enough, we loop to close all file descriptors
static auto stub_close_range(unsigned int first, unsigned int last, int flags) -> long {
#ifdef USE_FALLBACK_CLOSE_RANGE
  auto result = ::syscall(436, first, last, flags);
  if (result != -1 || errno != ENOSYS) {
    return result;
  }
  ::close(static_cast<int>(first));
  for (auto i = first + 1; i <= last && i > first; i++) {
    ::close(static_cast<int>(i));
  }
  return 0;
#else
  return ::close_range(first, last, flags);
#endif
}
} // namespace

namespace GPS {
void Logging::arrange_children() {
  // close and reopen stdout/stderr on the pipe
  if (this->pipes[1] != -1) {
    dup2(this->pipes[1], STDOUT_FILENO);
    dup2(this->pipes[1], STDERR_FILENO);
    close(pipes[1]);
  }
}
void Logging::arrange_parent(pid_t) {
  if (this->pipes[1] != -1) {
    close(this->pipes[1]);
  }
}
Logging::Logging(std::filesystem::path destination) : destination(std::move(destination)) {}
auto Logging::name() -> std::string_view { return "logging"; }
void Logging::before_fork() {
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
    stub_close_range(3, ~0U, CLOSE_RANGE_UNSHARE);
    execlp("tee", "tee", this->destination.c_str(), nullptr);
    // you shall not be here
    perror("cannot exec tee");
  }
  close(this->pipes[0]);
}
[[nodiscard]] auto Logging::get_log_path() const -> const std::filesystem::path & {
  return this->destination;
}

template <>
auto prepare_module<Logging>(Configurations &parser) -> std::function<std::unique_ptr<GLaunchModule>(
  const std::unordered_map<std::string, std::any> &arguments,
  const std::vector<std::string_view>             &raw_commandline
)> {
  parser.add_option("--log", Configurations::CommonParsers::identity_parser, 1);
  return [](const std::unordered_map<std::string, std::any> &arguments, const std::vector<std::string_view> &)
           -> std::unique_ptr<GLaunchModule> {
    if (!arguments.contains("log")) {
      return {nullptr};
    }
    const auto logging_path = std::any_cast<std::string>(arguments.at("log"));
    return std::make_unique<Logging>(logging_path);
  };
}

template <> void show_help<Logging>() {
  std::println("  --log PATH                 Duplicate and save stdout and stderr to PATH               ");
  std::println("                                                                                        ");
}
} // namespace GPS