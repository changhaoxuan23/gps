// glaunch-module - Definition of modules as part of the glaunch tool
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

#ifndef GPS_GLAUNCH_MODULE_HH_
#define GPS_GLAUNCH_MODULE_HH_
#include <configuration.hh>
#include <glaunch-module-manager.hh>
#include <glaunch-protocols.hh>

#include <any>
#include <concepts>
#include <unistd.h>
#include <unordered_map>
namespace GPS {
// this concept declares the static interface of glaunch modules
template <typename T>
concept GLaunchModule = requires(
  Configurations                                  &parser,
  GLaunchModuleManagerBuilder                     &manager_builder,
  const std::unordered_map<std::string, std::any> &parsed_commandline,
  const std::vector<std::string_view>             &raw_commandline
) {
  // setup commandline options used in this module
  { T::install_parser(parser) } -> std::same_as<void>;

  // show help about option added by this module to stdout
  //  an empty line shall be included at the end of help lines
  { T::show_help() } -> std::same_as<void>;

  // register this module to the manager
  // note that the module can decide by itself if it should be registered, likely according to the arguments
  //  supplied via the commandline
  { T::register_module(manager_builder, parsed_commandline, raw_commandline) } -> std::same_as<void>;
};
} // namespace GPS
#endif