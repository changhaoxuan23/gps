// utils.hh - General utilities that may be useful across multiple programs
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

#ifndef GPS_UTILS_HH_
#define GPS_UTILS_HH_
#include <filesystem>
#include <optional>
#include <string_view>
namespace GPS {
// a loose approximation of the which command: this function searches under directories in PATH
//  and tries to find the absolute (but not necessarily canonical) path of the supplied program name
auto find_executable(std::string_view name) -> std::optional<std::filesystem::path>;
} // namespace GPS
#endif