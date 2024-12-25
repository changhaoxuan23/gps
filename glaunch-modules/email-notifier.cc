// glaunch-modules-email-notifier - Send notification via email for glaunch
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

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <email-notifier.hh>
#include <glaunch-modules/logging.hh>
#include <glaunch-modules/timing.hh>
#include <nvml_common.hh>
#include <utils.hh>

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <sys/mman.h>
#include <termios.h>

namespace {
#ifdef EMAIL_NOTIFIER_SHOULD_WORK
static auto find_email_sender() -> const std::filesystem::path & {
  static auto path = ([]() -> std::filesystem::path {
    auto query = GPS::find_executable("send-email.py");
    if (query) {
      return query.value();
    }
    return {};
  })();
  return path;
}
#endif
} // namespace

namespace GPS {
#ifdef EMAIL_NOTIFIER_SHOULD_WORK
EmailNotifier::EmailNotifier(
  std::string                        &&server,
  uint16_t                             port,
  key_serial_t                         account,
  key_serial_t                         password,
  const std::vector<std::string_view> &commandline,
  std::filesystem::path              &&script
)
  : email_server(server), email_port(port), email_key(account), password_key(password),
    commandline(commandline), script(script) {}
#else
EmailNotifier::EmailNotifier() = default;
#endif
auto EmailNotifier::name() -> std::string_view { return "email-notifier"; }

auto EmailNotifier::get_module_attribute() -> ModuleAttribute { return ModuleAttribute::KeepAlive; }

void EmailNotifier::after_program_exit(ExitStatus status) {
#ifdef EMAIL_NOTIFIER_SHOULD_WORK
#define give_up(error)                                                                                       \
  do {                                                                                                       \
    std::println(stderr, "cannot send email notification: {}", error);                                       \
    return;                                                                                                  \
  } while (false)
  std::ostringstream body;

  // one line summary
  std::array<char, 100> hostname;
  gethostname(hostname.data(), hostname.size());
  std::println(body, "The process launched by glaunch on {} has terminated.", hostname.data());
  std::print(body, "This program was launched with commandline [");
  {
    bool first = true;
    for (const auto token : this->commandline) {
      if (first) {
        first = false;
      } else {
        body << ' ';
      }
      body << std::quoted(token);
    }
  }
  std::println(body, "].");

  // explain the exit result
  if (status.exited_normally) {
    std::println(body, "The program exited with code {}", status.exit_code);
  } else if (status.killed_by_signal) {
    std::println(body, "The program killed by signal {}", status.signal_id);
  } else {
    std::println(body, "The program terminated due to unknown reason");
  }

  // include information from other modules
  auto attachments = PyList_New(0);
  for (const auto &module : get_all_modules()) {
    if (module->name() == "timing") {
      // include elapsed time
      std::println(
        body,
        "{} passed before the program terminates",
        get_readable_duration(static_cast<const Timing *>(module.get())->get_elapsed_time())
      );
    } else if (module->name() == "logging") {
      // include the log file
      const auto &log_path =
        std::filesystem::canonical(static_cast<const Logging *>(module.get())->get_log_path()).string();
      auto path_string =
        PyUnicode_FromStringAndSize(log_path.c_str(), static_cast<Py_ssize_t>(log_path.size()));
      PyList_Append(attachments, path_string);
      Py_DECREF(path_string);
    }
  }

  // send the email
  // 1. setup environment variable to import the script
  // 1.1. read current PYTHONPATH in
  std::string python_path{::getenv("PYTHONPATH")};
  // 1.2. add a separator if required
  if (!python_path.empty()) {
    python_path.push_back(':');
  }
  // 1.3. append the path we found send-email.py
  python_path.append(std::filesystem::canonical(this->script.parent_path()).string());
  // 1.4. update the environment variable
  ::setenv("PYTHONPATH", python_path.c_str(), 1);

  // 2. initialize Python environment
  // 2.1. initialize python
  Py_Initialize();
  // 2.2. load the script file
  auto module = PyImport_ImportModule("send-email");
  if (module == nullptr) {
    give_up("failed to import send-email.py");
  }
  // 2.3. load the function
  auto function = PyObject_GetAttrString(module, "do_send");
  if (function == nullptr || !PyCallable_Check(function)) {
    Py_DECREF(module);
    Py_XDECREF(function);
    give_up("failed to load function do_send");
  }
  // 2.4. build argument to call the function
  auto  arguments = PyTuple_New(8);
  void *buffer    = nullptr;
  long  size      = 0;
  // note that PyTuple_SetItem steals the ownership
  PyTuple_SetItem(
    arguments,
    0,
    PyUnicode_FromStringAndSize(
      this->email_server.c_str(), static_cast<Py_ssize_t>(this->email_server.size())
    )
  );
  PyTuple_SetItem(arguments, 1, PyLong_FromLong(this->email_port));
  size = keyctl_read_alloc(this->email_key, &buffer);
  PyTuple_SetItem(arguments, 2, PyUnicode_FromStringAndSize(reinterpret_cast<char *>(buffer), size));
  free(buffer);
  size = keyctl_read_alloc(this->password_key, &buffer);
  PyTuple_SetItem(arguments, 3, PyUnicode_FromStringAndSize(reinterpret_cast<char *>(buffer), size));
  free(buffer);
  PyTuple_SetItem(
    arguments, 4, PyUnicode_FromString("Notification on termination of program launched by glaunch")
  );
  const auto body_string = body.str();
  PyTuple_SetItem(
    arguments,
    5,
    PyUnicode_FromStringAndSize(body_string.c_str(), static_cast<Py_ssize_t>(body_string.size()))
  );
  PyTuple_SetItem(
    arguments,
    6,
    PyUnicode_FromStringAndSize(
      this->email_server.c_str(), static_cast<Py_ssize_t>(this->email_server.size())
    )
  );
  PyTuple_SetItem(arguments, 7, attachments);
  // 3. call the function to send email
  auto return_value = PyObject_CallObject(function, arguments);
  if (return_value == Py_False) {
    std::println(stderr, "failed to send email notification");
  }
  // 4. cleanup
  Py_DECREF(arguments);
  Py_DECREF(return_value);
  Py_DECREF(function);
  Py_DECREF(module);
  Py_FinalizeEx();
#endif
}

template <>
auto prepare_module<EmailNotifier>(Configurations &parser)
  -> std::function<std::unique_ptr<GLaunchModule>(
    const std::unordered_map<std::string, std::any> &arguments,
    const std::vector<std::string_view>             &raw_commandline
  )> {
#ifdef EMAIL_NOTIFIER_SHOULD_WORK
  // check for send-email.py, we need that to actually send an email
  //  do not create the option if such script cannot be found
  auto path = find_email_sender();
  if (path.empty()) {
    return [](const std::unordered_map<std::string, std::any> &, const std::vector<std::string_view> &)
             -> std::unique_ptr<GLaunchModule> { return {nullptr}; };
  }
  parser.add_option("--email-notify", Configurations::CommonParsers::identity_parser, 1);
#endif

  return [&path](
           const std::unordered_map<std::string, std::any> &arguments,
           const std::vector<std::string_view>             &raw_commandline
         ) -> std::unique_ptr<GLaunchModule> {
#ifdef EMAIL_NOTIFIER_SHOULD_WORK
    constexpr auto email_description    = "email address";
    constexpr auto password_description = "email password";
    if (!arguments.contains("email-notify")) {
      return {nullptr};
    }
    // parse server and port from the commandline
    auto        server_string = std::any_cast<std::string>(arguments.at("email-notify"));
    std::string email_server;
    uint16_t    email_port;
    if (server_string.front() == '[') {
      auto stop_point = server_string.find(']');
      if (stop_point == server_string.npos) {
        std::println(stderr, "invalid server string: unmatched '['");
        exit(EXIT_FAILURE);
      }
      if (stop_point == server_string.size() - 1) {
        email_server = server_string.substr(1, server_string.size() - 2);
        email_port   = 465;
      } else {
        if (server_string.size() < stop_point + 3) {
          std::println(stderr, "invalid server string: insufficient length of port number");
          exit(EXIT_FAILURE);
        }
        if (server_string[stop_point + 1] != ':') {
          std::println(stderr, "invalid server string: expected ':' before port number");
          exit(EXIT_FAILURE);
        }
        email_port =
          Configurations::CommonParsers::full_convert_unsigned<uint16_t>(server_string.substr(stop_point + 2)
          );
        email_server = server_string.substr(1, stop_point - 1);
      }
    } else {
      if (server_string.find(':') != server_string.rfind(':')) {
        std::println(
          stderr, "invalid server string: more than one ':' detected, enclose IPv6 address with []"
        );
        exit(EXIT_FAILURE);
      }
      auto stop_point = server_string.find(':');
      if (stop_point == server_string.npos) {
        email_server = server_string;
        email_port   = 465;
      } else {
        email_port =
          Configurations::CommonParsers::full_convert_unsigned<uint16_t>(server_string.substr(stop_point + 1)
          );
        email_server = server_string.substr(0, stop_point);
      }
    }

    // =======================================================================================================

    // ask for email account and password
    // stop terminal from echoing input
    termios current_config, new_config;
    tcgetattr(STDIN_FILENO, &current_config);
    new_config = current_config;
    new_config.c_lflag &= ~(ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &new_config);

    // allocate memory that will not be swapped out or read
    auto   page_size = sysconf(_SC_PAGE_SIZE);
    size_t size      = (4096 + page_size - 1) / page_size * page_size;

    auto buffer = mmap(nullptr, size, PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mlock(buffer, size);

    // ask for email account
    std::print("Enter your email account: ");
    std::cin.getline(reinterpret_cast<char *>(buffer), static_cast<std::streamsize>(size));
    auto email_key =
      add_key("user", email_description, buffer, std::cin.gcount() - 1, KEY_SPEC_PROCESS_KEYRING);

    // ask for email password
    std::print("Enter your email password: ");
    std::cin.getline(reinterpret_cast<char *>(buffer), static_cast<std::streamsize>(size));
    auto password_key =
      add_key("user", password_description, buffer, std::cin.gcount() - 1, KEY_SPEC_THREAD_KEYRING);

    // zero out and free memory
    explicit_bzero(buffer, size);
    munmap(buffer, size);

    // create the module instance
    return std::make_unique<EmailNotifier>(
      std::move(email_server), email_port, email_key, password_key, raw_commandline, std::move(path)
    );
#else
    return {nullptr};
#endif
  };
}
template <> void show_help<EmailNotifier>() {
#ifdef EMAIL_NOTIFIER_SHOULD_WORK
  if (find_email_sender().empty()) {
    // do not appear in the help message if we cannot work
    return;
  }
  std::println("  --email-notify SERVER      Notify about the termination of process launched via email ");
  std::println("                              The address/domain name and port of SMTP server is given  ");
  std::println("                               via SERVER string, which is <address/domain name>[:port] ");
  std::println("                               the address/domain name is required and port is optional.");
  std::println("                              If a IPv6 address is specified, enclose it with [] so that");
  std::println("                               it will not be mistaken as port number.                  ");
  std::println("                              If port number is omitted, we defaults to 465, the default");
  std::println("                               port number of SMTP over TLS.                            ");
  std::println("                              Attention: we do not check the address supplied but will  ");
  std::println("                               pass it directly to the email sender.                    ");
  std::println("                              Note that due to security consideration, we support only  ");
  std::println("                               SMTP over TLS.                                           ");
  std::println("                              Address and password is designed to be asked interactively");
  std::println("                               to stop it from leaking via command line arguments, which");
  std::println("                               is usually visible to any other user on the same machine,");
  std::println("                               and are stored in the process keyring secured directly by");
  std::println("                               the Linux kernel until the very moment that the launched ");
  std::println("                               process terminates.                                      ");
  std::println("                              The notification will contain the arguments used to launch");
  std::println("                               the process, its pid, exit status and following fields:  ");
  std::println("                               if --time is specified, total wall-clock time it costs;  ");
  std::println("                               if --log is specified, the logging file as an attachment.");
  std::println("                                                                                        ");
#endif
}
} // namespace GPS