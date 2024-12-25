// glaunch-modules - Defination of modules as part of the glaunch tool
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

#ifndef GPS_GLAUNCH_MODULE_HH_
#define GPS_GLAUNCH_MODULE_HH_
#include <configuration.hh>

#include <concepts>
#include <cstdint>
#include <forward_list>
#include <functional>
#include <memory>
#include <unistd.h>
namespace GPS {
struct ExitStatus {
  int     raw_status;
  uint8_t exited_normally : 1;
  uint8_t killed_by_signal : 1;
  union {
    int exit_code;
    int signal_id;
  };

  explicit ExitStatus(int status);
};

class GLaunchModule {
public:
  enum ModuleAttribute : uint32_t {
    Empty     = 0x0u,
    // this module should be kept alive, which means that this module have code that runs after the program
    //  is launched. In practice, this means that glaunch shall not do a direct execute, but fork before
    //  executing the program to be launched to give this module change to work as expected
    KeepAlive = 0x1u,
  };
  GLaunchModule();
  virtual ~GLaunchModule();
  virtual auto name() -> std::string_view = 0;
  // called right before fork(2) is called to make the process to run the program to be launched
  virtual void before_fork();

  // called right before exec* is called, with the return value of which passed as argument
  void before_exec(pid_t pid);

  // get attribute of this module
  virtual auto get_module_attribute() -> ModuleAttribute;

  // called right after the program launched is terminated
  //  any module that overrides this method must also override get_module_attribute
  //  to return attribute with KeepAlive set
  virtual void after_program_exit(ExitStatus status);

protected:
  // called in the child process right after fork(2) is called
  virtual void arrange_children();
  // called in the parent process right after fork(2) is called with the pid of child process as argument
  virtual void arrange_parent(pid_t pid);
};

// setup commandline options and callback to utilize the arguments collected
//  the parser is supplied to this function so that this function can set option parsers
//  this function shall return a callback function which is called when the commandline is parsed
//   this callback function takes the parsed result, an std::unordered_map
//                                the raw comandline, an std::vector of std::string_view(s)
//    as its argument
//   the callback function shall return an instance of std::unique_ptr, which either holds a nullptr,
//    in case a module shall not be created, or an instance of class derived from GLaunchModule
template <std::derived_from<GLaunchModule> Module>
auto prepare_module(Configurations &parser) -> std::function<std::unique_ptr<GLaunchModule>(
                                              const std::unordered_map<std::string, std::any> &arguments,
                                              const std::vector<std::string_view>             &raw_commandline
                                            )>;

// show help about option added by this module to stdout
//  an empty line shall be included at the end of help lines
template <std::derived_from<GLaunchModule> Module> void show_help();

// getter to get all active modules
auto get_all_modules() -> const std::forward_list<std::unique_ptr<GLaunchModule>> &;
} // namespace GPS
#endif