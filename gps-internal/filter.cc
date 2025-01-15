// gps-internal:filter - Filter to select processes
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

#include <algorithm>
#include <array>
#include <cerrno>
#include <concepts>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filter.hh>
#include <forward_list>
#include <functional>
#include <iomanip>
#include <iostream>
#include <print>
#include <queue>
#include <regex>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>
#include <vector>

namespace {
using GPS::process_information;
using GPS::Filter::Filter;

// meta filters: logical operations
class Not : public Filter {
public:
  Not(std::unique_ptr<Filter> &&filter);

  [[nodiscard]] auto evaluate(const process_information &process) const -> bool override;

  static void help();
#ifndef NDEBUG
  void print(unsigned int indent = 0) const override;
#endif

private:
  std::unique_ptr<Filter> filter;
};
class And : public Filter {
public:
  And(std::forward_list<std::unique_ptr<Filter>> &&filters);

  [[nodiscard]] auto evaluate(const process_information &process) const -> bool override;

  static void help();
#ifndef NDEBUG
  void print(unsigned int indent = 0) const override;
#endif

private:
  const std::forward_list<std::unique_ptr<Filter>> filters;
};
class Or : public Filter {
public:
  Or(std::forward_list<std::unique_ptr<Filter>> &&filters);

  [[nodiscard]] auto evaluate(const process_information &process) const -> bool override;

  static void help();
#ifndef NDEBUG
  void print(unsigned int indent = 0) const override;
#endif

private:
  const std::forward_list<std::unique_ptr<Filter>> filters;
};

// all filters
template <typename T>
concept AllFilters = requires {
  std::derived_from<Filter, T>;
  std::same_as<std::invoke_result_t<decltype(T::help)>, void>;
};

// real filters
struct Environment {
  bool have_operation{false};
};

static auto get_registered_filters()
  -> std::forward_list<std::function<std::unique_ptr<Filter>(std::queue<std::string_view> &, Environment &)>>
    & {
  static std::forward_list<
    std::function<std::unique_ptr<Filter>(std::queue<std::string_view> &, Environment &)>>
    list;
  return list;
}

template <typename T>
concept RealFilters = requires {
  AllFilters<T>;
  std::convertible_to<
    decltype(T::build),
    std::function<std::unique_ptr<Filter>(std::queue<std::string_view> &, Environment &)>>;
};

template <RealFilters T> static void register_filter() { get_registered_filters().emplace_front(T::build); }

// match the process ID (pid)
class ProcessID : public Filter {
public:
  static auto        build(std::queue<std::string_view> &args, Environment &env) -> std::unique_ptr<Filter>;
  [[nodiscard]] auto evaluate(const process_information &process) const -> bool override;

  static void help();
#ifndef NDEBUG
  void print(unsigned int indent = 0) const override;
#endif

private:
  pid_t pid;
};
// match the (real) user ID
class RealUserID : public Filter {
public:
  static auto        build(std::queue<std::string_view> &args, Environment &env) -> std::unique_ptr<Filter>;
  [[nodiscard]] auto evaluate(const process_information &process) const -> bool override;

  static void help();
#ifndef NDEBUG
  void print(unsigned int indent = 0) const override;
#endif

private:
  uid_t uid;
};

// operations
// show help message
class Help : public Filter {
public:
  static auto        build(std::queue<std::string_view> &args, Environment &env) -> std::unique_ptr<Filter>;
  [[nodiscard]] auto evaluate(const process_information &process) const -> bool override;

  static void help();
#ifndef NDEBUG
  void print(unsigned int indent = 0) const override;
#endif
};
// print process information
class Print : public Filter {
public:
  static auto        build(std::queue<std::string_view> &args, Environment &env) -> std::unique_ptr<Filter>;
  [[nodiscard]] auto evaluate(const process_information &process) const -> bool override;

  static void help();
#ifndef NDEBUG
  void print(unsigned int indent = 0) const override;
#endif
};
// print process information in custom format
class Format : public Filter {
public:
  static auto        build(std::queue<std::string_view> &args, Environment &env) -> std::unique_ptr<Filter>;
  [[nodiscard]] auto evaluate(const process_information &process) const -> bool override;

  static void help();
#ifndef NDEBUG
  void print(unsigned int indent = 0) const override;
#endif
};
// execute command on the process
class Execute : public Filter {
public:
  static auto        build(std::queue<std::string_view> &args, Environment &env) -> std::unique_ptr<Filter>;
  [[nodiscard]] auto evaluate(const process_information &process) const -> bool override;

  static void help();
#ifndef NDEBUG
  void print(unsigned int indent = 0) const override;
#endif

private:
  std::vector<std::string_view> arguments;
};
// kill the process
class Kill : public Filter {
public:
  static auto        build(std::queue<std::string_view> &args, Environment &env) -> std::unique_ptr<Filter>;
  [[nodiscard]] auto evaluate(const process_information &process) const -> bool override;

  static void help();
#ifndef NDEBUG
  void print(unsigned int indent = 0) const override;
#endif

private:
  int signal;
};
} // namespace

namespace {
Not::Not(std::unique_ptr<Filter> &&filter) : filter(std::move(filter)) {}
auto Not::evaluate(const process_information &process) const -> bool {
  return !this->filter->evaluate(process);
}
void Not::help() {}
#ifndef NDEBUG
void Not::print(unsigned int indent) const {
  std::println(stderr, "{}Not{{", std::string(indent * 2ul, ' '));
  this->filter->print(indent + 1);
  std::println(stderr, "{}}}", std::string(indent * 2ul, ' '));
}
#endif

And::And(std::forward_list<std::unique_ptr<Filter>> &&filters) : filters(std::move(filters)) {}
auto And::evaluate(const process_information &process) const -> bool {
  for (const auto &filter : this->filters) {
    if (!filter->evaluate(process)) {
      return false;
    }
  }
  return true;
}
void And::help() {}
#ifndef NDEBUG
void And::print(unsigned int indent) const {
  std::println(stderr, "{}And{{", std::string(indent * 2ul, ' '));
  for (const auto &filter : this->filters) {
    filter->print(indent + 1);
  }
  std::println(stderr, "{}}}", std::string(indent * 2ul, ' '));
}
#endif

Or::Or(std::forward_list<std::unique_ptr<Filter>> &&filters) : filters(std::move(filters)) {}
auto Or::evaluate(const process_information &process) const -> bool {
  for (const auto &filter : this->filters) {
    if (filter->evaluate(process)) {
      return true;
    }
  }
  return false;
}
void Or::help() {}
#ifndef NDEBUG
void Or::print(unsigned int indent) const {
  std::println(stderr, "{}Or{{", std::string(indent * 2ul, ' '));
  for (const auto &filter : this->filters) {
    filter->print(indent + 1);
  }
  std::println(stderr, "{}}}", std::string(indent * 2ul, ' '));
}
#endif

auto ProcessID::build(std::queue<std::string_view> &args, Environment &) -> std::unique_ptr<Filter> {
  bool is_pid = false;
  if (args.front() == "-pid") {
    args.pop();
    is_pid = true;
  }
  if (!args.empty() && std::ranges::all_of(args.front(), [](const char c) -> bool { return ::isdigit(c); })) {
    auto filter = std::make_unique<ProcessID>();
    filter->pid = std::stoi(std::string{args.front()});
    args.pop();
    return filter;
  } else if (is_pid) {
    // pid expected but the supplied value does not look like one
    std::println(stderr, "Invalid argument to -pid: it does not look like a numerical PID");
    ::exit(EXIT_FAILURE);
  }
  return {nullptr};
}
auto ProcessID::evaluate(const process_information &process) const -> bool {
  return process.pid == this->pid;
}
void ProcessID::help() {}
#ifndef NDEBUG
void ProcessID::print(unsigned int indent) const {
  std::println(stderr, "{}pid == {}", std::string(indent * 2ul, ' '), this->pid);
}
#endif

auto RealUserID::build(std::queue<std::string_view> &args, Environment &) -> std::unique_ptr<Filter> {
  if (args.front() != "-user" && args.front() != "-ruid" && args.front() != "-uid") {
    return {nullptr};
  }

  const auto name = args.front();
  args.pop();
  if (args.empty() || !std::ranges::all_of(args.front(), [](const char c) -> bool { return ::isdigit(c); })) {
    std::println(stderr, "Invalid argument to {}: it does not look like a numerical UID", name);
    ::exit(EXIT_FAILURE);
  }

  auto filter = std::make_unique<RealUserID>();
  filter->uid = std::stoi(std::string{args.front()});
  args.pop();
  return filter;
}
auto RealUserID::evaluate(const process_information &process) const -> bool {
  return process.uids.real_uid == this->uid;
}
void RealUserID::help() {}
#ifndef NDEBUG
void RealUserID::print(unsigned int indent) const {
  std::println(stderr, "{}ruid == {}", std::string(indent * 2ul, ' '), this->uid);
}
#endif

auto Help::build(std::queue<std::string_view> &args, Environment &) -> std::unique_ptr<Filter> {
  if (args.front() != "-help") {
    return {nullptr};
  }
  std::println("");
  Or::help();
  And::help();
  Not::help();
  ProcessID::help();
  RealUserID::help();
  Print::help();
  Format::help();
  Execute::help();
  Kill::help();
  Help::help();

  ::exit(EXIT_SUCCESS);
}
auto Help::evaluate(const process_information &) const -> bool {
  // no-op: this function shall never be called
  return true;
}
void Help::help() {}
#ifndef NDEBUG
void Help::print(unsigned int indent) const {
  std::println(stderr, "{}HELP", std::string(indent * 2ul, ' '));
}
#endif

auto Print::build(std::queue<std::string_view> &args, Environment &env) -> std::unique_ptr<Filter> {
  if (args.front() == "-print") {
    env.have_operation = true;
    args.pop();
    return std::make_unique<Print>();
  }
  return {nullptr};
}
auto Print::evaluate(const process_information &process) const -> bool {
  // commandline
  std::print("[{}] ", process.pid);
  if (process.args.size() == 0) {
    std::println("unknown command line");
  } else {
    for (const auto &part : process.args) {
      std::cout << std::quoted(part) << ' ';
    }
    putchar('\n');
  }

  // owner/permission information
  std::println("[{}]   Owner:", process.pid);
  std::println(
    "[{}]     Effective UID:  {} ({})", process.pid, process.uids.effective_uid, process.uids.effective_login
  );
  std::println(
    "[{}]     Real UID:       {} ({})", process.pid, process.uids.real_uid, process.uids.real_login
  );
  std::println(
    "[{}]     Saved UID:      {} ({})", process.pid, process.uids.saved_uid, process.uids.saved_login
  );
  std::println(
    "[{}]     Filesystem UID: {} ({})",
    process.pid,
    process.uids.filesystem_uid,
    process.uids.filesystem_login
  );

  // timing information
  std::println("[{}]   Timing:", process.pid);
  std::println(
    stderr,
    "[{}]     Usermode:    {} second(s) ({})",
    process.pid,
    process.timing.usermode_seconds,
    get_readable_duration(process.timing.usermode_seconds)
  );
  std::println(
    stderr,
    "[{}]     Kernelmode:  {} second(s) ({})",
    process.pid,
    process.timing.kernelmode_seconds,
    get_readable_duration(process.timing.kernelmode_seconds)
  );
  std::println(
    stderr,
    "[{}]     Wall-clock:  {} second(s) ({})",
    process.pid,
    process.timing.elapsed_seconds,
    get_readable_duration(process.timing.elapsed_seconds)
  );

  // CPU information
  std::println(stderr, "[{}]   CPU memory: {}", process.pid, get_readable_size(process.cpu_memory));

  // GPU information
  unsigned long long total_gpu_memory = 0;
  for (const auto &device : process.devices) {
    total_gpu_memory += device.memory_used;
  }
  std::println(
    stderr,
    "[{}]   GPU memory: running on {} devices, {} in use",
    process.pid,
    process.devices.size(),
    get_readable_size(total_gpu_memory)
  );
  for (const auto &device : process.devices) {
    std::println(
      stderr,
      "[{}]     on device {} ({}): {} / {}, {:.3}%",
      process.pid,
      device.device.id,
      device.device.name,
      get_readable_size(device.memory_used),
      get_readable_size(device.device.memory.total),
      static_cast<double>(device.memory_used) / static_cast<double>(device.device.memory.total) * 100
    );
  }
  putchar('\n');

  return true;
}
void Print::help() {}
#ifndef NDEBUG
void Print::print(unsigned int indent) const {
  std::println(stderr, "{}PRINT", std::string(indent * 2ul, ' '));
}
#endif

auto Format::build(std::queue<std::string_view> &args, Environment &env) -> std::unique_ptr<Filter> {
  if (args.front() == "-format") {
    env.have_operation = true;
    args.pop();
    std::println(stderr, "sorry, but -format is not yet implemented. We will replace this with -print.");
    return std::make_unique<Print>();
  }
  return {nullptr};
}
auto Format::evaluate([[maybe_unused]] const process_information &process) const -> bool { return true; }
void Format::help() {}
#ifndef NDEBUG
void Format::print(unsigned int indent) const {
  std::println(stderr, "{}FORMAT", std::string(indent * 2ul, ' '));
}
#endif

auto Execute::build(std::queue<std::string_view> &args, Environment &env) -> std::unique_ptr<Filter> {
  if (args.front() != "-exec" && args.front() != "-execute") {
    return {nullptr};
  }

  const auto name = args.front();
  args.pop();

  env.have_operation = true;

  std::vector<std::string_view> arguments;

  while (!args.empty()) {
    if (args.front() == ";") {
      if (arguments.empty()) {
        std::println(stderr, "cannot execute empty command in {}", name);
        ::exit(EXIT_FAILURE);
      }
      auto filter       = std::make_unique<Execute>();
      filter->arguments = std::move(arguments);
      args.pop();
      return filter;
    }

    arguments.emplace_back(args.front());
    args.pop();
  }
  std::println(
    stderr,
    "list of arguments not terminated properly for {}. You should terminate it with `;'.\nDo note that this "
    "may require escaping since the shell may take that as an special character.",
    name
  );
  ::exit(EXIT_FAILURE);
}
auto Execute::evaluate(const process_information &process) const -> bool {
  // build commandline
  static std::regex pid_replacer(R"(\[:pid:\])");

  std::forward_list<std::string> commandline_holder;
  std::vector<char *>            commandline;
  for (const auto argument : this->arguments) {
    commandline_holder.emplace_front(
      std::regex_replace(std::string(argument), pid_replacer, std::to_string(process.pid))
    );
    commandline.emplace_back(commandline_holder.front().data());
  }
  commandline.emplace_back(nullptr);

  auto pid = ::fork();
  if (pid == 0) {
    ::execvp(commandline.front(), commandline.data());
    std::println(stderr, "failed to execute command: {}", strerror(errno));
    ::exit(EXIT_FAILURE);
  }

  int status;
  ::waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
void Execute::help() {}
#ifndef NDEBUG
void Execute::print(unsigned int indent) const {
  std::print(stderr, "{}EXECUTE(", std::string(indent * 2ul, ' '));
  for (const auto item : this->arguments) {
    std::cerr << std::quoted(item) << " ";
  }
  std::println(")");
}
#endif

auto Kill::build(std::queue<std::string_view> &args, Environment &env) -> std::unique_ptr<Filter> {
  static const auto signals = ([]() {
    std::array<const char *, NSIG> signals;
    for (int i = 0; i < NSIG; i++) {
#if __GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 32)
      signals[i] = sys_siglist[i];
#else
      signals[i] = sigabbrev_np(i);
#endif
    }
    return signals;
  })();

  if (args.front() != "-kill") {
    return {nullptr};
  }
  args.pop();
  env.have_operation = true;

  int signal = SIGTERM;

  if (!args.empty() && !args.front().starts_with('-')) {
    if (::isdigit(args.front()[0])) {
      signal = std::stoi(std::string(args.front()));
    } else {
      std::string name(args.front());
      std::ranges::for_each(name, [](char &c) { c = static_cast<char>(::toupper(c)); });
      if (name.starts_with("SIG")) {
        name = name.substr(3);
      }

      bool found = false;
      for (int i = 0; i < static_cast<int>(signals.size()); i++) {
        if (signals[i] == nullptr) {
          continue;
        }
        if (name == signals[i]) {
          signal = i;
          found  = true;
          break;
        }
      }

      if (!found) {
        std::println(stderr, "unrecognized signal name SIG{}", name);
        ::exit(EXIT_FAILURE);
      }
    }
    args.pop();
  }

  auto filter    = std::make_unique<Kill>();
  filter->signal = signal;
  return filter;
}
auto Kill::evaluate(const process_information &process) const -> bool {
  return ::kill(process.pid, this->signal) == 0;
}
void Kill::help() {}
#ifndef NDEBUG
void Kill::print(unsigned int indent) const {
  std::println(stderr, "{}KILL(sig={})", std::string(indent * 2ul, ' '), this->signal);
}
#endif
} // namespace

namespace {
// a not-very-formal syntax defintion about the filter expression supplied as the commandline
//  FilterExpression      ::= OrFilterExpression
//                          | \epsilon
//  OrFilterExpression    ::= AndFilterExpression [ '-or' OrFilterExpression ]
//  AndFilterExpression   ::= UnaryFilterExpression [ ['-and'] AndFilterExpression ]
//  UnaryFilterExpression ::= '-not' OrFilterExpression
//                          | RealFilterExpression
//                          | '(' FilterExpression ')'
//  RealFilterExpression  ::= '-'...

// entry point of the parser
static auto build_filter_expression(std::queue<std::string_view> &args, Environment &env)
  -> std::unique_ptr<Filter>;
static auto build_or_expression(std::queue<std::string_view> &args, Environment &env)
  -> std::unique_ptr<Filter>;
static auto build_and_expression(std::queue<std::string_view> &args, Environment &env)
  -> std::unique_ptr<Filter>;
static auto build_unary_expression(std::queue<std::string_view> &args, Environment &env)
  -> std::unique_ptr<Filter>;
static auto build_real_expression(std::queue<std::string_view> &args, Environment &env)
  -> std::unique_ptr<Filter>;

static auto build_filter_expression(std::queue<std::string_view> &args, Environment &env)
  -> std::unique_ptr<Filter> {
  if (args.empty()) {
    env.have_operation = true;
    return std::make_unique<Print>();
  }
  return build_or_expression(args, env);
}
static auto build_or_expression(std::queue<std::string_view> &args, Environment &env)
  -> std::unique_ptr<Filter> {
  std::forward_list<std::unique_ptr<Filter>> filters;
  auto                                       tail = filters.before_begin();
  do {
    if (args.front() == "-or") {
      if (tail == filters.before_begin()) {
        std::println(stderr, "warning: do not prefix expression with `-or'");
      }
      args.pop();
    }
    tail = filters.emplace_after(tail, build_and_expression(args, env));
  } while (!args.empty() && args.front() == "-or");
  // filter shall never be empty here: the program shall have been terminated if build_and_expression fails
  if (tail != filters.begin()) {
    // more than one filter, construct a real or node
    return std::make_unique<Or>(std::move(filters));
  }
  return std::move(filters.front());
}
static auto build_and_expression(std::queue<std::string_view> &args, Environment &env)
  -> std::unique_ptr<Filter> {
  std::forward_list<std::unique_ptr<Filter>> filters;
  auto                                       tail = filters.before_begin();
  do {
    if (args.front() == "-and") {
      if (tail == filters.before_begin()) {
        std::println(stderr, "warning: do not prefix expression with `-and'");
      }
      args.pop();
    }
    tail = filters.emplace_after(tail, build_unary_expression(args, env));
  } while (!args.empty() && args.front() != "-or" && args.front() != ")");
  // filter shall never be empty here: the program shall have been terminated if build_unary_expression fails
  if (tail != filters.begin()) {
    // more than one filter, construct a real or node
    return std::make_unique<And>(std::move(filters));
  }
  return std::move(filters.front());
}
static auto build_unary_expression(std::queue<std::string_view> &args, Environment &env)
  -> std::unique_ptr<Filter> {
  if (args.front() == "-not") {
    args.pop();
    return std::make_unique<Not>(build_or_expression(args, env));
  } else if (args.front() == "(") {
    args.pop();
    auto result = build_filter_expression(args, env);
    if (args.empty() || args.front() != ")") {
      std::println(stderr, "unpaired parentheses");
      ::exit(EXIT_FAILURE);
    }
    args.pop();
    return result;
  } else {
    return build_real_expression(args, env);
  }
}
static auto build_real_expression(std::queue<std::string_view> &args, Environment &env)
  -> std::unique_ptr<Filter> {
  // register all filters on the first entry
  [[maybe_unused]] static bool initialized = ([]() -> bool {
    register_filter<ProcessID>();
    register_filter<RealUserID>();
    register_filter<Help>();
    register_filter<Print>();
    register_filter<Format>();
    register_filter<Execute>();
    register_filter<Kill>();
    return true;
  })();
  if (args.empty()) {
    std::println(stderr, "unexpected empty list of tokens when building real filter");
    ::exit(EXIT_FAILURE);
  }

  auto result = std::unique_ptr<Filter>();
  for (const auto &builder : get_registered_filters()) {
    result = builder(args, env);
    if (result) {
      return result;
    }
  }
  std::println(stderr, "unrecognized filter starting with {}", args.front());
  ::exit(EXIT_FAILURE);
}

} // namespace

namespace GPS::Filter {
Filter::~Filter() = default;

auto build_filter(int argc, char **argv) -> std::unique_ptr<Filter> {
  std::queue<std::string_view> args(argv + 1, argv + argc);
  Environment                  env;
  auto                         result = build_filter_expression(args, env);
  if (!env.have_operation) {
    // construct a default print operation
    std::forward_list<std::unique_ptr<Filter>> filters;
    filters.emplace_front(std::move(result));
    filters.emplace_after(filters.cbegin(), std::make_unique<Print>());
    result = std::make_unique<And>(std::move(filters));
  }
  return result;
}
} // namespace GPS::Filter