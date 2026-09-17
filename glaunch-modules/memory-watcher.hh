// glaunch-modules-memory-watcher - Monitor GPU memory usage for glaunch
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

#ifndef GPS_GLAUNCH_MODULE_MEMORY_WATCHER_HH_
#define GPS_GLAUNCH_MODULE_MEMORY_WATCHER_HH_

#include <glaunch-module.hh>
#include <nvml_common.hh>

#include <format>
namespace GPS {

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
  inline auto operator=(enum_type rhs) -> MemoryMonitoringMethod & {
    this->value = rhs;
    return *this;
  }
  inline      operator bool() const { return this->value != MemoryMonitoringMethod::NONE; }
  inline auto operator|(enum_type rhs) const -> MemoryMonitoringMethod {
    return MemoryMonitoringMethod(static_cast<enum_type>(this->value | rhs));
  }
  inline auto operator|(const MemoryMonitoringMethod &rhs) const -> MemoryMonitoringMethod {
    return this->operator|(rhs.value);
  }
  inline auto operator&(enum_type rhs) const -> MemoryMonitoringMethod {
    return MemoryMonitoringMethod(static_cast<enum_type>(this->value & rhs));
  }
  inline auto operator&(const MemoryMonitoringMethod &rhs) const -> MemoryMonitoringMethod {
    return this->operator&(rhs.value);
  }
  inline auto operator~() const -> MemoryMonitoringMethod {
    return MemoryMonitoringMethod(static_cast<enum_type>(MemoryMonitoringMethod::ALL - this->value));
  }
  inline auto operator|=(enum_type rhs) -> MemoryMonitoringMethod & {
    this->value = static_cast<enum_type>(this->value | rhs);
    return *this;
  }
  inline auto operator|=(const MemoryMonitoringMethod &rhs) -> MemoryMonitoringMethod & {
    return this->operator|=(rhs.value);
  }
  inline auto operator&=(enum_type rhs) -> MemoryMonitoringMethod & {
    this->value = static_cast<enum_type>(this->value & rhs);
    return *this;
  }
  inline auto operator&=(const MemoryMonitoringMethod &rhs) -> MemoryMonitoringMethod & {
    return this->operator&=(rhs.value);
  }

private:
  enum_type value{MemoryMonitoringMethod::NONE};
};

class MemoryWatcher : public GLaunchModule {
private:
  int                    pipes[2];
  MemoryMonitoringMethod method;
  pid_t                  pid;

  struct parsed_trace_line {
    std::string_view api_name;
    std::string_view arguments;
    bool             succeed;

    parsed_trace_line(std::string_view line);
  };

  static auto get_time() -> std::string;

  void nvml_memory_watcher();

  void trace_memory_watcher();

public:
  MemoryWatcher(MemoryMonitoringMethod method);
  void before_fork() override;
  auto name() -> std::string_view override;

  auto get_module_attribute() -> ModuleAttribute override;

protected:
  void arrange_children() override;
  void arrange_parent(pid_t pid) override;
};

template <>
auto prepare_module<MemoryWatcher>(Configurations &parser)
  -> std::function<std::unique_ptr<GLaunchModule>(
    const std::unordered_map<std::string, std::any> &arguments,
    const std::vector<std::string_view>             &raw_commandline
  )>;
template <> void show_help<MemoryWatcher>();
} // namespace GPS
template <> struct std::formatter<GPS::MemoryMonitoringMethod> {
  constexpr auto parse(std::format_parse_context &ctx) { return ctx.begin(); }
  template <std::output_iterator<char> Iter>
  auto format(const GPS::MemoryMonitoringMethod &v, std::basic_format_context<Iter, char> &ctx) const {
    constexpr auto keys  = std::to_array<std::pair<const char *, GPS::MemoryMonitoringMethod::enum_type>>({
      {"nvml", GPS::MemoryMonitoringMethod::NVML},
      {"trace", GPS::MemoryMonitoringMethod::Trace},
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
#endif