// glaunch-modules-email-notifier - Send notification via email for glaunch
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

#ifndef GPS_GLAUNCH_MODULE_EMAIL_NOTIFIER_HH_
#define GPS_GLAUNCH_MODULE_EMAIL_NOTIFIER_HH_
#include <glaunch-module.hh>

#ifdef EMAIL_NOTIFIER_SHOULD_WORK
#include <filesystem>
#include <keyutils.h>
#endif

namespace GPS {
class EmailNotifier : public GLaunchModule {
public:
#ifdef EMAIL_NOTIFIER_SHOULD_WORK
  EmailNotifier(
    std::string                        &&server,
    uint16_t                             port,
    key_serial_t                         account,
    key_serial_t                         password,
    const std::vector<std::string_view> &commandline,
    std::filesystem::path              &&script
  );
#else
  EmailNotifier();
#endif
  auto name() -> std::string_view override;

  auto get_module_attribute() -> ModuleAttribute override;

  void after_program_exit(ExitStatus status) override;

private:
#ifdef EMAIL_NOTIFIER_SHOULD_WORK
  std::string                         email_server;
  uint16_t                            email_port;
  key_serial_t                        email_key    = -1;
  key_serial_t                        password_key = -1;
  const std::vector<std::string_view> commandline;
  const std::filesystem::path         script;
#endif
};
template <>
auto prepare_module<EmailNotifier>(Configurations &parser)
  -> std::function<std::unique_ptr<GLaunchModule>(
    const std::unordered_map<std::string, std::any> &arguments,
    const std::vector<std::string_view>             &raw_commandline
  )>;
template <> void show_help<EmailNotifier>();
} // namespace GPS
#endif