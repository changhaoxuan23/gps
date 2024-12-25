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

#include <nvml_common.hh>
#include <timing.hh>
namespace GPS {
auto Timing::get_module_attribute() -> ModuleAttribute { return ModuleAttribute::KeepAlive; }
auto Timing::name() -> std::string_view { return "timing"; }
void Timing::after_program_exit(ExitStatus) {
  clock_gettime(CLOCK_MONOTONIC, &this->end_time);
  const auto total_time  = this->get_elapsed_time();
  const auto time_string = get_readable_duration(total_time);
  std::println(stderr, "elapsed time: {}", time_string);
}

auto Timing::get_elapsed_time() const -> uintmax_t {
  uintmax_t total_time = this->end_time.tv_sec - this->start_time.tv_sec;
  return total_time;
}

void Timing::arrange_parent(pid_t) { clock_gettime(CLOCK_MONOTONIC, &this->start_time); }

template <>
auto prepare_module<Timing>(Configurations &parser)
  -> std::function<std::unique_ptr<GLaunchModule>(
    const std::unordered_map<std::string, std::any> &arguments,
    const std::vector<std::string_view>             &raw_commandline
  )> {
  parser.add_option("--time", Configurations::CommonParsers::true_parser, 0);
  return [](const std::unordered_map<std::string, std::any> &arguments, const std::vector<std::string_view> &)
           -> std::unique_ptr<GLaunchModule> {
    if (!arguments.contains("time")) {
      return {nullptr};
    }
    return std::make_unique<Timing>();
  };
}
template <> void show_help<Timing>() {
  std::println("  --time                     When the program terminates, summary its elapsed time      ");
  std::println("                                                                                        ");
}
} // namespace GPS