// glaunch-protocols - standard protocols of glaunch modules
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

#ifndef GPS_GLAUNCH_MODULE_PROTOCOLS_HH_
#define GPS_GLAUNCH_MODULE_PROTOCOLS_HH_

#include <any>
#include <ctime>
#include <expected>
#include <filesystem>
#include <forward_list>
#include <functional>
#include <string>
#include <string_view>

namespace GPS {
// helper structures for passing values around

// a module may either return what request or an error, which should be described in string
//  the name of the module will be packed along side with it
template <typename T> struct protocol_result_t {
  std::string_view              module_name;
  std::expected<T, std::string> value;
};

// result returned from individual modules will be batched by the manager with this batcher
template <typename... T> using protocol_result_batcher_t = std::forward_list<T...>;

// handler function of protocol calls
//  since custom protocols are also supported, we use std::any to pass arguments packed in std::tuple around
//  the return value is some specializations of the protocol_result_t wrapped in std::any
using protocol_handler_t = std::function<std::any(std::string_view, const std::any &)>;

// -----------------------------------------------------------------------------------------------------------

// definition of protocol ids
using protocol_id_t = const char *const;
namespace ProtocolID {
constexpr const char GetLoggingFiles[]      = "$.get_logging_files";
constexpr const char GetTimingInformation[] = "$.get_timing_information";
}; // namespace ProtocolID
template <protocol_id_t protocol_id> struct Protocol {
  static inline constexpr const bool is_standard = false;
};
// -----------------------------------------------------------------------------------------------------------

// common helpers for accessing information about standard protocols

// if the specified protocol id declared as a standard protocol
template <protocol_id_t protocol_id>
inline constexpr bool is_standard_protocol = Protocol<protocol_id>::is_standard;

// the callable type of the protocol
template <protocol_id_t protocol_id>
using standard_protocol_callable_t = Protocol<protocol_id>::callable_type;

// result type of the called protocol
template <protocol_id_t protocol_id>
using standard_protocol_result_t =
  protocol_result_t<typename standard_protocol_callable_t<protocol_id>::return_type>;
// batched result of calling the protocol: this will be returned by the manager since multiple modules may
//  have implemented the same protocol and returned their result individually
template <protocol_id_t protocol_id>
using standard_protocol_batched_result_t = protocol_result_batcher_t<standard_protocol_result_t<protocol_id>>;

// -----------------------------------------------------------------------------------------------------------

// information of standard protocols

// $.get_logging_files: retrieve the path to logged file
template <> struct Protocol<ProtocolID::GetLoggingFiles> {
  static inline constexpr const bool is_standard = true;
  using callable_type                            = std::function<const std::filesystem::path &()>;
};

// $.get_timing_information: get elapsed time recorded
template <> struct Protocol<ProtocolID::GetTimingInformation> {
  static inline constexpr const bool is_standard = true;
  using callable_type                            = std::function<timespec()>;
};
} // namespace GPS
#endif