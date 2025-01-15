// utils.cc - General utilities that may be useful across multiple programs
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

#include <utils.hh>

#include <cstdlib>
#include <pwd.h>
#include <ranges>
#include <string>
#include <unistd.h>

auto GPS::find_executable(std::string_view name) -> std::optional<std::filesystem::path> {
  std::string paths{getenv("PATH")};
  for (const auto path : std::views::split(paths, ":")) {
    auto target{std::filesystem::path(std::string_view(path)) / name};
    if (std::filesystem::is_regular_file(target)) {
      return {target};
    }
  }
  return {};
}

auto GPS::get_pagesize() -> size_t {
  static size_t page_size = sysconf(_SC_PAGESIZE);
  return page_size;
}

auto GPS::to_username(uid_t uid) -> std::string {
  const auto pwd = getpwuid(uid);
  return pwd == nullptr ? "" : pwd->pw_name;
}