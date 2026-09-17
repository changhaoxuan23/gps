// glaunch-hooks - hooks available for glaunch modules
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

#ifndef GPS_GLAUNCH_MODULE_HOOKS_HH_
#define GPS_GLAUNCH_MODULE_HOOKS_HH_

#include <ctime>
#include <functional>
#include <type_traits>
#include <utility>

namespace GPS {
// definition of hook ids
enum class HookID : uint8_t {
  BeforeFork,
  AfterParentFork,
  BeforeExec,
  AfterExit,
  HOOK_COUNT_,
};
constexpr const auto HookCount = static_cast<std::underlying_type_t<HookID>>(HookID::HOOK_COUNT_);

// helping structure for hook callable typing
template <HookID hook_id> struct Hook {};

// common helpers for accessing information about hooks
// the callable type of the hook. note that hooks always return nothing
template <HookID hook_id> using hook_callable_t = Hook<hook_id>::callable_type;

// types used in hooks
// the exit status of a process, which is a structured representation of data received via wait call
struct ExitStatus {
  // the raw status code returned from wait
  int     raw_status;
  // if the process exited normally, same as WIFEXITED(this->raw_status)
  uint8_t exited_normally : 1;
  // if the process was killed by some signal, same as WIFSIGNALED(this->raw_status)
  uint8_t killed_by_signal : 1;
  union {
    // exit code of the process, effective only when this->exited_normally evaluates to true
    //  same as WEXITSTATUS(this->raw_status)
    int exit_code;
    // signal number used to kill the process, effective only when this->killed_by_signal evaluates to true
    //  same as WTERMSIG(this->raw_status)
    int signal_id;
  };

  explicit ExitStatus(int status);
};

// hook declarations
template <> struct Hook<HookID::BeforeFork> {
  using callable_type = std::function<void()>;
};
template <> struct Hook<HookID::AfterParentFork> {
  using callable_type = std::function<void(pid_t)>;
};
template <> struct Hook<HookID::BeforeExec> {
  using callable_type = std::function<void()>;
};
template <> struct Hook<HookID::AfterExit> {
  using callable_type = std::function<void(ExitStatus)>;
};

// helpers that ensures all declared hooks has callable of the same size
template <typename T, T... ids> constexpr auto check_hook_callable_size(std::integer_sequence<T, ids...>) {
  return (
    (sizeof(hook_callable_t<static_cast<HookID>(0)>) == sizeof(hook_callable_t<static_cast<HookID>(ids)>))
    && ...
  );
}
static_assert(
  check_hook_callable_size(std::make_integer_sequence<std::underlying_type_t<HookID>, HookCount>{}),
  "callable of all hooks must share the same size"
);
constexpr const auto HookCallableSize = sizeof(hook_callable_t<static_cast<HookID>(0)>);
} // namespace GPS
#endif