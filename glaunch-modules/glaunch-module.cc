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

#include <glaunch-module.hh>

#include <sys/wait.h>
namespace GPS {
ExitStatus::ExitStatus(int status) : raw_status(status) {
  this->exited_normally  = WIFEXITED(status);
  this->killed_by_signal = WIFSIGNALED(status);
  if (this->exited_normally) {
    this->exit_code = WEXITSTATUS(status);
  } else if (this->killed_by_signal) {
    this->signal_id = WTERMSIG(status);
  }
}

GLaunchModule::GLaunchModule()  = default;
GLaunchModule::~GLaunchModule() = default;
void GLaunchModule::before_fork() {
  // no-op in base class
}
void GLaunchModule::before_exec(pid_t pid) {
  // dispatch according to the pid
  if (pid == 0) {
    this->arrange_children();
  } else {
    this->arrange_parent(pid);
  }
}
auto GLaunchModule::get_module_attribute() -> ModuleAttribute { return ModuleAttribute::Empty; }
void GLaunchModule::after_program_exit(ExitStatus) {
  // no-op in base class
}
void GLaunchModule::arrange_children() {
  // no-op in base class
};
void GLaunchModule::arrange_parent(pid_t){
  // no-op in base class
};
} // namespace GPS