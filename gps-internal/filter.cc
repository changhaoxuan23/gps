// gps-internal:filter - Filter to select processes
// Copyright (C) 2025 Haoxuan Chang<changhaoxuan23@mails.ucas.ac.cn>

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
#include <filter.hh>

#include <algorithm>
#include <array>
#include <cerrno>
#include <concepts>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <forward_list>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <print>
#include <pwd.h>
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

// match the number of GPUs used
class GPUCount : public Filter {
public:
  static auto        build(std::queue<std::string_view> &args, Environment &env) -> std::unique_ptr<Filter>;
  [[nodiscard]] auto evaluate(const process_information &process) const -> bool override;

  static void help();
#ifndef NDEBUG
  void print(unsigned int indent = 0) const override;
#endif

private:
  // match processes running on [minimum, maximum] GPUs
  uint16_t minimum;
  uint16_t maximum;
};

// match the amount of GPU memory used
class GPUMemory : public Filter {
public:
  static auto        build(std::queue<std::string_view> &args, Environment &env) -> std::unique_ptr<Filter>;
  [[nodiscard]] auto evaluate(const process_information &process) const -> bool override;

  static void help();
#ifndef NDEBUG
  void print(unsigned int indent = 0) const override;
#endif

private:
  // match processes utilizing [minimum, maximum] bytes of GPU memory
  size_t minimum;
  size_t maximum;
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
// utility functions
template <std::unsigned_integral T> static auto apply_range_modifier(std::string_view modifier, T value) {
  struct {
    T    minimum;
    T    maximum;
    bool reversed;
  } result;

  if (modifier == "eq") {
    result.reversed = false;
    result.minimum  = value;
    result.maximum  = value;
  } else if (modifier == "ne") {
    result.reversed = true;
    result.minimum  = value;
    result.maximum  = value;
  } else if (modifier == "lt") {
    result.reversed = false;
    result.minimum  = 0;
    result.maximum  = value - 1;
  } else if (modifier == "le") {
    result.reversed = false;
    result.minimum  = 0;
    result.maximum  = value;
  } else if (modifier == "gt") {
    result.reversed = false;
    result.minimum  = value + 1;
    result.maximum  = std::numeric_limits<T>::max();
  } else if (modifier == "ge") {
    result.reversed = false;
    result.minimum  = value;
    result.maximum  = std::numeric_limits<T>::max();
  } else {
    std::println(stderr, "unrecognized modifier [{}]", modifier);
    ::exit(EXIT_FAILURE);
  }
  return result;
}
} // namespace

namespace {
Not::Not(std::unique_ptr<Filter> &&filter) : filter(std::move(filter)) {}
auto Not::evaluate(const process_information &process) const -> bool {
  return !this->filter->evaluate(process);
}
void Not::help() {
  std::println("  -not <expression>                     Logical not. This component will evaluate to true  ");
  std::println("                                         if and only if the expression evaluates to false. ");
  std::println("                                                                                           ");
}
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
void And::help() {
  std::println("  <expression> -and <expression>        Logical and. This component will evaluate to true  ");
  std::println("                                         if and only if both of the  two expressions would ");
  std::println("                                         evaluate to true.                                 ");
  std::println("                                        This is the default conjunction between components.");
  std::println("                                         <e1> -and <e2> will be evaluated in identical way ");
  std::println("                                         as <e1> <e2>.                                     ");
  std::println("                                        Note that this component follows the Short-circuit ");
  std::println("                                         evaluation scheme: if the left hand side results  ");
  std::println("                                         in false, the right hand side is not evaluated.   ");
  std::println("                                                                                           ");
}
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
void Or::help() {
  std::println("  <expression> -or <expression>         Logical or. This component evaluates to true if any");
  std::println("                                         one of the two expressions evaluates to true. If  ");
  std::println("                                         both side evaluates to false, this component will ");
  std::println("                                         result in generating false.                       ");
  std::println("                                        Note that this component follows the Short-circuit ");
  std::println("                                         evaluation scheme: if the left hand side results  ");
  std::println("                                         in true, the right hand side is not evaluated.    ");
  std::println("                                                                                           ");
}
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
void ProcessID::help() {
  std::println("  [-pid ]<PID>                          Match the PID of processes. This component requests");
  std::println("                                         an exact match between the PID of process which is");
  std::println("                                         being evaluated and the value supplied.           ");
  std::println("                                        The component name part (`-pid') can be omitted.   ");
  std::println("                                                                                           ");
}
#ifndef NDEBUG
void ProcessID::print(unsigned int indent) const {
  std::println(stderr, "{}pid == {}", std::string(indent * 2ul, ' '), this->pid);
}
#endif

auto RealUserID::build(std::queue<std::string_view> &args, Environment &) -> std::unique_ptr<Filter> {
  bool is_username = false;
  if (args.front() == "-user:name" || args.front() == "-ruid:name" || args.front() == "-uid:name") {
    is_username = true;
  } else if (args.front() != "-user" && args.front() != "-ruid" && args.front() != "-uid") {
    return {nullptr};
  }

  const auto name = args.front();
  args.pop();
  if (args.empty()) {
    std::println(stderr, "{} requires exactly one argument but none is supplied", name);
    ::exit(EXIT_FAILURE);
  }

  if (!is_username) {
    is_username = !std::ranges::all_of(args.front(), [](const char c) -> bool { return ::isdigit(c); });
  }

  auto filter = std::make_unique<RealUserID>();
  if (is_username) {
    auto pwd = ::getpwnam(static_cast<const char *>(args.front().data()));
    if (pwd == nullptr) {
      std::println(stderr, "{}: cannot find user with name {}", name, args.front());
      ::exit(EXIT_FAILURE);
    }
    filter->uid = pwd->pw_uid;
  } else {
    filter->uid = std::stoi(std::string{args.front()});
  }

  args.pop();
  return filter;
}
auto RealUserID::evaluate(const process_information &process) const -> bool {
  return process.uids.real_uid == this->uid;
}
void RealUserID::help() {
  std::println("  -user[:name] <UID>                    Match the real user ID of processes. This requires ");
  std::println("  -ruid[:name] <UID>                     an exact match between the real UID of the process");
  std::println("  -uid[:name]  <UID>                     being evaluated and the value supplied.           ");
  std::println("                                        The UID can be supplied as the user ID like 1000 or");
  std::println("                                         the username like foobar. In case the parser fails");
  std::println("                                         to recognize the type of UID automatically, suffix");
  std::println("                                         the component name with `:name' so that the parser");
  std::println("                                         will interpret the argument as username regardless");
  std::println("                                         how it looks like.                                ");
  std::println("                                                                                           ");
}
#ifndef NDEBUG
void RealUserID::print(unsigned int indent) const {
  std::println(stderr, "{}ruid == {}", std::string(indent * 2ul, ' '), this->uid);
}
#endif

auto GPUCount::build(std::queue<std::string_view> &args, Environment &) -> std::unique_ptr<Filter> {
  if (args.front() != "-gpus") {
    return {nullptr};
  }
  args.pop();

  if (args.empty()) {
    std::println(stderr, "-gpus requires exactly one argument but none is supplied");
    ::exit(EXIT_FAILURE);
  }

  auto start = args.front().find_first_of("0123456789");
  if (start == args.front().npos) {
    std::println(stderr, "invalid argument to -gpus");
    ::exit(EXIT_FAILURE);
  }
  auto     modifier = start == 0 ? std::string_view{"eq"} : args.front().substr(0, start);
  uint16_t value    = std::stoul(std::string(args.front().substr(start)));
  auto     result   = apply_range_modifier(modifier, value);
  args.pop();
  auto filter     = std::make_unique<GPUCount>();
  filter->minimum = result.minimum;
  filter->maximum = result.maximum;
  if (result.reversed) {
    return std::make_unique<Not>(std::move(filter));
  }
  return filter;
}
auto GPUCount::evaluate(const process_information &process) const -> bool {
  return process.devices.size() <= this->maximum && process.devices.size() >= this->minimum;
}
void GPUCount::help() {
  std::println("  -gpus [<modifier>]<number>            Match the number of GPUs being used by the process.");
  std::println("                                        Modifiers may be used to change the way numbers are");
  std::println("                                         interpreted. Modifiers known to this component are");
  std::println("                                         listed in the following table.                    ");
  std::println("                                         --------------------------------------------------");
  std::println("                                          MODIFIER    MEANING                              ");
  std::println("                                         --------------------------------------------------");
  std::println("                                             eq       Exact match with the number          ");
  std::println("                                             ne       Exactly not matched by the number    ");
  std::println("                                             lt       less than the number                 ");
  std::println("                                             le       not more than the number             ");
  std::println("                                             gt       more than the number                 ");
  std::println("                                             ge       not less than the number             ");
  std::println("                                         --------------------------------------------------");
  std::println("                                        If no modifier is supplied, assume `eq'.           ");
  std::println("                                                                                           ");
}
#ifndef NDEBUG
void GPUCount::print(unsigned int indent) const {
  std::println(
    stderr, "{}{} <= GPU count <= {}", std::string(indent * 2ul, ' '), this->minimum, this->maximum
  );
}
#endif

auto GPUMemory::build(std::queue<std::string_view> &args, Environment &) -> std::unique_ptr<Filter> {
  if (args.front() != "-gpu-memory") {
    return {nullptr};
  }
  args.pop();

  if (args.empty()) {
    std::println(stderr, "-gpu-memory requires exactly one argument but none is supplied");
    ::exit(EXIT_FAILURE);
  }

  auto start = args.front().find_first_of("0123456789");
  if (start == args.front().npos) {
    std::println(stderr, "invalid argument to -gpu-memory");
    ::exit(EXIT_FAILURE);
  }
  auto                     modifier = start == 0 ? std::string_view{"eq"} : args.front().substr(0, start);
  std::vector<std::string> helper;
  helper.emplace_back(args.front().substr(start));
  auto value =
    std::any_cast<unsigned long long>(Configurations::CommonParsers::size_parser(helper.begin(), helper.end())
    );
  auto result = apply_range_modifier(modifier, value);
  args.pop();
  auto filter     = std::make_unique<GPUMemory>();
  filter->minimum = result.minimum;
  filter->maximum = result.maximum;
  if (result.reversed) {
    return std::make_unique<Not>(std::move(filter));
  }
  return filter;
}
auto GPUMemory::evaluate(const process_information &process) const -> bool {
  size_t total_memory = 0;
  std::ranges::for_each(
    process.devices.cbegin(),
    process.devices.cend(),
    [&total_memory](const process_information::host_device &device) { total_memory += device.memory_used; }
  );
  return total_memory <= this->maximum && total_memory >= this->minimum;
}
void GPUMemory::help() {
  std::println("  -gpu-memory [<modifier>]<number>      Match the amount of GPU memory used by the process.");
  std::println("                                         Size is measured by bytes.                        ");
  std::println("                                        Modifiers may be used to change the way numbers are");
  std::println("                                         interpreted. See -gpus for modifiers available.   ");
  std::println("                                        Suffixing modifiers are available to make it easier");
  std::println("                                         when specifying large amount. See the table below:");
  std::println("                                         --------------------------------------------------");
  std::println("                                          MODIFIER    MULTIPLIER                           ");
  std::println("                                         --------------------------------------------------");
  std::println("                                             k        2^10                                 ");
  std::println("                                             kb       2^10                                 ");
  std::println("                                             kib      2^10                                 ");
  std::println("                                             m        2^20                                 ");
  std::println("                                             mb       2^20                                 ");
  std::println("                                             mib      2^20                                 ");
  std::println("                                             g        2^30                                 ");
  std::println("                                             gb       2^30                                 ");
  std::println("                                             gib      2^30                                 ");
  std::println("                                             t        2^40                                 ");
  std::println("                                             tb       2^40                                 ");
  std::println("                                             tib      2^40                                 ");
  std::println("                                             p        2^50                                 ");
  std::println("                                             pb       2^50                                 ");
  std::println("                                             pib      2^50                                 ");
  std::println("                                         --------------------------------------------------");
  std::println("                                         These suffixing modifiers are case insensitive.   ");
  std::println("                                                                                           ");
}
#ifndef NDEBUG
void GPUMemory::print(unsigned int indent) const {
  std::println(
    stderr, "{}{} <= GPU memory <= {}", std::string(indent * 2ul, ' '), this->minimum, this->maximum
  );
}
#endif

auto Help::build(std::queue<std::string_view> &args, Environment &) -> std::unique_ptr<Filter> {
  if (args.front() != "-help" && args.front() != "--help" && args.front() != "-h") {
    return {nullptr};
  }
  std::println("gps, GPU ps, lists computational processes currently running on local system.              ");
  std::println("                                                                                           ");
  std::println("USAGE                                                                                      ");
  std::println("  gps [filter-expression]                                                                  ");
  std::println("                                                                                           ");
  std::println(" The filter expression is evaluated against each and every computational processes found.  ");
  std::println(" The order of processes is not specified: process with smaller PID does not necessarily be ");
  std::println("  evaluated against before one with larger PID.                                            ");
  std::println(" Parentheses are allowed in the expression to GROUP SUB EXPRESSIONS. Note that parentheses ");
  std::println("  can be special characters for shells, you may need to escape them so that they will not  ");
  std::println("  be mistaken.                                                                             ");
  std::println("                                                                                           ");
  std::println("EXPRESSION COMPONENTS                                                                      ");
  std::println("                                                                                           ");
  Or::help();
  And::help();
  Not::help();
  ProcessID::help();
  RealUserID::help();
  GPUCount::help();
  GPUMemory::help();
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
void Help::help() {
  std::println("  -help                                 Print this help message again. Return nothing since");
  std::println("  --help                                 this component will terminate the program.        ");
  std::println("  -h                                    This is an operation.                              ");
  std::println("                                                                                           ");
}
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
    if (process.cmdline_tailing_null_bytes != 0) {
      std::print("[{} tailing null bytes, cmdline may have been erased]", process.cmdline_tailing_null_bytes);
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
    "[{}]     Usermode:    {} second(s) ({})",
    process.pid,
    process.timing.usermode_seconds,
    get_readable_duration(process.timing.usermode_seconds)
  );
  std::println(
    "[{}]     Kernelmode:  {} second(s) ({})",
    process.pid,
    process.timing.kernelmode_seconds,
    get_readable_duration(process.timing.kernelmode_seconds)
  );
  std::println(
    "[{}]     Wall-clock:  {} second(s) ({})",
    process.pid,
    process.timing.elapsed_seconds,
    get_readable_duration(process.timing.elapsed_seconds)
  );

  // CPU information
  std::println("[{}]   CPU memory: {}", process.pid, get_readable_size(process.cpu_memory));

  // GPU information
  unsigned long long total_gpu_memory = 0;
  for (const auto &device : process.devices) {
    total_gpu_memory += device.memory_used;
  }
  std::println(
    "[{}]   GPU memory: running on {} devices, {} in use",
    process.pid,
    process.devices.size(),
    get_readable_size(total_gpu_memory)
  );
  for (const auto &device : process.devices) {
    std::println(
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
void Print::help() {
  std::println("  -print                                Print information about the process being evaluated");
  std::println("                                         at this moment in the default format, then return ");
  std::println("                                         true as its result.                               ");
  std::println("                                        This is an operation.                              ");
  std::println("                                        Print is the default operation: if no operation is ");
  std::println("                                         configured, -print will be added automatically by ");
  std::println("                                         changing                                          ");
  std::println("                                           <full-expression>                               ");
  std::println("                                          into                                             ");
  std::println("                                           (<full-expression>) -and -print                 ");
  std::println("                                        The output format of -print is not specified but it");
  std::println("                                         is designed to be read by human beings. The format");
  std::println("                                         may change without any prior notice, so don't make");
  std::println("                                         any assumptions about the specific format. If you ");
  std::println("                                         need to write a script, consider -format, -exec or");
  std::println("                                         something similar.                                ");
  std::println("                                                                                           ");
}
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
void Format::help() {
  std::println("  -format                               Print information about the process being evaluated");
  std::println("                                         at the moment following the formatting instruction");
  std::println("                                         supplied, then return true as its result.         ");
  std::println("                                        This is an operation.                              ");
  std::println("                                        This operation is not yet implemented, currently it");
  std::println("                                         work in exactly the same way as -print.           ");
  std::println("                                                                                           ");
}
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
void Execute::help() {
  std::println("  -exec    <command> ;                  Execute command with attributes of the process that");
  std::println("  -execute <command> ;                   is being evaluated for now. Return the exit status");
  std::println("                                         of command being executed, true for succeed.      ");
  std::println("                                        This is an operation.                              ");
  std::println("                                        Anything after component name and before the `;' is");
  std::println("                                         is taken to construct command to be executed. Any ");
  std::println("                                         occurrence of certain character sequence will be  ");
  std::println("                                         replaced following specification in the following ");
  std::println("                                         table.                                            ");
  std::println("                                          -------------------------------------------------");
  std::println("                                          SEQUENCE               REPLACED BY               ");
  std::println("                                          -------------------------------------------------");
  std::println("                                           [:pid:]               the PID of the process    ");
  std::println("                                          -------------------------------------------------");
  std::println("                                        Note that the command is not executed in any shell,");
  std::println("                                         which means that shell aliases, shell builtins and");
  std::println("                                         variable will not work. Multiple instances of exec");
  std::println("                                         is definitely allowed, but, as can be inferred by ");
  std::println("                                         the information aforementioned, their environment ");
  std::println("                                         is not shared. If you want shell support, create a");
  std::println("                                         script and run it with -exec.                     ");
  std::println("                                         supplied, then return true as its result.         ");
  std::println("                                        The character `;' can be special in shells, you may");
  std::println("                                         need to escape it so that it can be passed to gps.");
  std::println("                                                                                           ");
}
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
void Kill::help() {
  std::println("  -kill [<SIGNAL>]                      Kill the process currently being evaluated. Returns");
  std::println("                                         if the kill(2) syscall is made successfully.      ");
  std::println("                                        This is an operation.                              ");
  std::println("                                        An optional argument may be supplied to specify the");
  std::println("                                         signal to be sent to the proccess. Default to TERM");
  std::println("                                         if which is not supplied.                         ");
  std::println("                                        The signal can be specified by:                    ");
  std::println("                                          its code,       like -kill 9                     ");
  std::println("                                          its full name,  like -kill SIGKILL               ");
  std::println("                                          its short name, like -kill KILL                  ");
  std::println("                                                                                           ");
}
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
    register_filter<GPUCount>();
    register_filter<GPUMemory>();
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