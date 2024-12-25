// glaunch-modules-timing - Time the launched program for glaunch
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

#ifndef GPS_GLAUNCH_MODULE_TIMING_HH_
#define GPS_GLAUNCH_MODULE_TIMING_HH_
#include <glaunch-module.hh>

#include <ctime>
namespace GPS {
class Timing : public GLaunchModule {
public:
  auto get_module_attribute() -> ModuleAttribute override;
  auto name() -> std::string_view override;

  void after_program_exit(ExitStatus status) override;

  [[nodiscard]] auto get_elapsed_time() const -> uintmax_t;

protected:
  void arrange_parent(pid_t pid) override;

private:
  timespec start_time;
  timespec end_time;
};

template <>
auto prepare_module<Timing>(Configurations &parser)
  -> std::function<std::unique_ptr<GLaunchModule>(
    const std::unordered_map<std::string, std::any> &arguments,
    const std::vector<std::string_view>             &raw_commandline
  )>;
template <> void show_help<Timing>();
} // namespace GPS
#endif