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

#ifndef GPS_GLAUNCH_MODULE_LOGGING_HH_
#define GPS_GLAUNCH_MODULE_LOGGING_HH_
#include <glaunch-module.hh>

#include <filesystem>
namespace GPS {
class Logging : public GLaunchModule {
  // setup logging: we use tee to do this job, assuming which is installed on the system
  //  since it is part of the GNU coreutils, it shall be safe to make such assumption in common cases
private:
  int                         pipes[2];
  const std::filesystem::path destination;

protected:
  void arrange_children() override;
  void arrange_parent(pid_t) override;

public:
  Logging(std::filesystem::path destination);
  auto name() -> std::string_view override;
  void before_fork() override;

  [[nodiscard]] auto get_log_path() const -> const std::filesystem::path &;
};

template <>
auto prepare_module<Logging>(Configurations &parser)
  -> std::function<std::unique_ptr<GLaunchModule>(
    const std::unordered_map<std::string, std::any> &arguments,
    const std::vector<std::string_view>             &raw_commandline
  )>;
template <> void show_help<Logging>();
} // namespace GPS
#endif