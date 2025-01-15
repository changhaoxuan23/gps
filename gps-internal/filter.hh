// gps-internal:filter - Filter to select processes
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

#ifndef GPS_GPS_INTERNAL_FILTER_HH_
#define GPS_GPS_INTERNAL_FILTER_HH_
#include <gps-internal/process_information.hh>

#include <memory>
namespace GPS::Filter {
class Filter {
public:
  virtual ~Filter();

  [[nodiscard]] virtual auto evaluate(const process_information &process) const -> bool = 0;
#ifndef NDEBUG
  virtual void print(unsigned int indent = 0) const = 0;
#endif
};

// build filter from commandline
auto build_filter(int argc, char **argv) -> std::unique_ptr<Filter>;
}; // namespace GPS::Filter
#endif