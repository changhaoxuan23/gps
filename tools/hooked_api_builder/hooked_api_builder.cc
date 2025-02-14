// hooked_api_builder - Build hooked version of CUDA API from the header of CUDA
// availability Copyright (C) 2024-2025 Haoxuan Chang<changhaoxuan23@mails.ucas.ac.cn>

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

#include <algorithm>
#include <clang-c/CXFile.h>
#include <clang-c/CXString.h>
#include <clang-c/Index.h>
#include <concepts>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <print>
#include <string_view>
#include <unordered_set>
#include <vector>

struct VisitorData {
  std::ofstream output_file;
  const std::unordered_set<std::string> &implemented_functions;
};

struct Argument {
  std::string name;
  CXType      type;

  Argument(std::string &&name, CXType &&type) : name(std::move(name)), type(type) {}
};

static auto cxs(CXString &&string) -> std::string {
  std::string result{clang_getCString(string)};
  clang_disposeString(string);
  return result;
}

static auto evaluate_structure(std::string_view name, const std::string &type_name)
  -> std::pair<std::string, std::string> {
  if (type_name == "cudaExtent") {
    return {"{{depth={}, height={}, width={}}}", std::format("{0}.depth, {0}.height, {0}.width", name)};
  } else {
    std::println(stderr, "TODO: unhandled structure: [{}]", type_name);
    return {std::format("<structure <{}>: not supported>", type_name), ""};
  }
}

static auto evaluate_value(std::string_view name, CXType type) -> std::pair<std::string, std::string> {
  constexpr auto char_types = std::to_array(
    {CXType_Char_U, CXType_UChar, CXType_Char16, CXType_Char32, CXType_Char_S, CXType_SChar, CXType_WChar}
  );
  if (type.kind == CXType_Void) {
    return {"", ""};
  }
  if (type.kind == CXType_Elaborated) {
    type = clang_Type_getNamedType(type);
  }
  auto underlying_type = clang_getCanonicalType(type);
  if (underlying_type.kind == CXType_Enum) {
    if (cxs(clang_getTypeSpelling(type)) == "cudaError_t") {
      return {
        "{} ({}: {})",
        std::format("static_cast<int>({0}), cudaGetErrorName({0}), cudaGetErrorString({0})", name)
      };
    } else {
      std::println(stderr, "TODO: unhandled enumerate type: [{}]", cxs(clang_getTypeSpelling(type)));
      return {"{}", std::format("static_cast<int>({})", name)};
    }
  }
  if (underlying_type.kind == CXType_Pointer
      && std::ranges::find(char_types, clang_getPointeeType(underlying_type).kind) == char_types.cend()) {
    return {"0x{:016x}", std::format("reinterpret_cast<uintmax_t>({})", name)};
  }
  if (underlying_type.kind == CXType_Record) {
    return evaluate_structure(name, cxs(clang_getTypeSpelling(underlying_type)));
  }
  return {"{}", std::string(name)};
}

static auto visitor(CXCursor current_cursor, CXCursor, CXClientData data_) -> CXChildVisitResult {
  auto kind = clang_getCursorKind(current_cursor);
  if (kind != CXCursor_FunctionDecl) {
    return CXChildVisit_Recurse;
  }
  auto function_name = cxs(clang_getCursorSpelling(current_cursor));
  if (!function_name.starts_with("cuda")) {
    return CXChildVisit_Continue;
  }
  auto &data = *reinterpret_cast<VisitorData *>(data_);
  if (function_name == "cudaGetErrorName" || function_name == "cudaGetErrorString"
      || data.implemented_functions.contains(function_name)) {
    return CXChildVisit_Continue;
  }
  auto &output      = data.output_file;
  auto  return_type = clang_getCursorResultType(current_cursor);

  if (return_type.kind == CXType_Void) {
    std::print(output, "void ");
  } else {
    std::print(output, "auto ");
  }
  std::print(output, "{}(", function_name);
  std::vector<Argument> arguments;
  for (int i = 0; i < clang_Cursor_getNumArguments(current_cursor); i++) {
    auto argument_ = clang_Cursor_getArgument(current_cursor, i);
    arguments.emplace_back(cxs(clang_getCursorSpelling(argument_)), clang_getCursorType(argument_));
    auto &argument = arguments.back();
    if (i != 0) {
      std::print(output, ", ");
    }
    std::print(output, "{} {}", cxs(clang_getTypeSpelling(argument.type)), argument.name);
  }
  std::print(output, ")");
  if (return_type.kind != CXType_Void) {
    std::print(output, " -> {}", cxs(clang_getTypeSpelling(return_type)));
  }
  std::println(output, "{{");

  std::println(
    output,
    "  static const auto real_{0} = reinterpret_cast<decltype(&{0})>(get_cuda_function(\"{0}\"));",
    function_name
  );
  std::println(output, "  static const auto [mask, require_trace] = get_mask(\"{}\");\n", function_name);
  std::print(output, "  handle_output(mask, require_trace, std::format(\"{}(", function_name);
  std::string suffix;
  for (const auto &argument : arguments) {
    auto real_type = argument.type;
    if (real_type.kind == CXType_Elaborated) {
      real_type = clang_Type_getNamedType(real_type);
    }
    real_type = clang_getCanonicalType(real_type);

    if (&argument != arguments.data()) {
      std::print(output, ", ");
    }
    std::print(output, "{}=", argument.name);
    const auto &[here, there] = evaluate_value(argument.name, argument.type);
    std::print(output, "{}", here);
    if (!suffix.empty() && !there.empty()) {
      suffix.append(", ");
    }
    suffix.append(there);
  }
  std::println(output, ")\"{}{}), true);", suffix.empty() ? "" : ", ", suffix);

  std::print(output, "  ");
  if (return_type.kind != CXType_Void) {
    std::print(output, "auto trace_internal_result = ");
  }

  std::print(output, "real_{}(", function_name);
  for (auto iter = arguments.cbegin(); iter != arguments.cend(); iter++) {
    if (iter != arguments.cbegin()) {
      std::print(output, ", ");
    }
    std::print(output, "{}", iter->name);
  }
  std::println(output, ");");

  if (return_type.kind != CXType_Void) {
    const auto &[here, there] = evaluate_value("trace_internal_result", return_type);
    std::println(
      output,
      R"(  handle_output(mask, require_trace, std::format(" -> {}\n"{}{}), false);)",
      here,
      there.empty() ? "" : ", ",
      there
    );
  } else {
    std::println(output, R"(  handle_output(mask, require_trace, "  DONE\n", false);)");
  }

  std::println(output, "  return trace_internal_result;");
  std::println(output, "}}");

  return CXChildVisit_Recurse;
}

template <std::invocable<> T> struct scoped_deleter {
  scoped_deleter(const T &deleter) : deleter_(deleter) {}
  scoped_deleter(const scoped_deleter &)                          = delete;
  scoped_deleter(scoped_deleter &&other)                          = delete;
  auto operator=(const scoped_deleter &other) -> scoped_deleter & = delete;
  auto operator=(scoped_deleter &&other) -> scoped_deleter      & = delete;
  ~scoped_deleter() { this->deleter_(); }

private:
  const T &deleter_;
};

// some function have been implemented manually in hooked_api_head.cc
//  we shall not generate implementations for these functions
//  parse this source file and scan for them, return an unordered_set containing name
//  of all functions implemented manually
static auto get_implemented_functions(const std::filesystem::path &source
) -> std::unordered_set<std::string> {
  auto index = clang_createIndex(0, 0);
  if (index == nullptr) {
    return {};
  }
  scoped_deleter index_deleter([index]() { clang_disposeIndex(index); });
  const auto     args = std::to_array<const char *>({"-DSCANNING"});
  auto           unit = clang_parseTranslationUnit(
    index,
    source.c_str(),
    args.data(),
    args.size(),
    nullptr,
    0,
    CXTranslationUnit_SkipFunctionBodies | CXTranslationUnit_KeepGoing
  );
  if (unit == nullptr) {
    return {};
  }
  scoped_deleter                  unit_deleter([unit]() { clang_disposeTranslationUnit(unit); });
  std::unordered_set<std::string> result;
  clang_visitChildren(
    clang_getTranslationUnitCursor(unit),
    [](CXCursor target, CXCursor, CXClientData data) -> CXChildVisitResult {
      auto &result = *reinterpret_cast<std::unordered_set<std::string> *>(data);
      auto  kind   = clang_getCursorKind(target);
      if (kind != CXCursor_FunctionDecl) {
        return CXChildVisit_Recurse;
      }
      auto function_name = cxs(clang_getCursorSpelling(target));
      if (!function_name.starts_with("cuda")) {
        return CXChildVisit_Recurse;
      }
      result.emplace(function_name);
      std::println(
        stderr,
        "hooked_api_builder: found manually implemented API << {} >>",
        cxs(clang_getCursorDisplayName(target))
      );
      return CXChildVisit_Recurse;
    },
    &result
  );
  return result;
}

auto main(int argc, char **argv) -> int {
  if (argc != 5) {
    std::println(
      stderr,
      "invalid usage. call with {} <input> <header> <preprocessed-header> <output> to build hooked APIs",
      argv[0]
    );
    std::println(stderr, "  <input>              : path to preprocessed CUDA API headers");
    std::println(stderr, "  <header>             : path to hooked_api_header.cc");
    std::println(stderr, "  <preprocessed-header>: path to preprocessed hooked_api_header.cc");
    std::println(stderr, "  <output>             : path to write output");
    exit(EXIT_FAILURE);
  }
  const std::filesystem::path input(argv[1]);
  const std::filesystem::path header(argv[2]);
  const std::filesystem::path preprocessed_header(argv[3]);
  const std::filesystem::path output(argv[4]);

  // load head file
  const auto &implemented_functions = get_implemented_functions(preprocessed_header);

  // create destination
  if (std::filesystem::is_regular_file(output)) {
    std::filesystem::remove(output);
  }
  std::filesystem::copy(header, output);

  // prepare calling user datas
  VisitorData data{std::ofstream{output, std::ios::app}, implemented_functions};
  data.output_file.put('\n');

  // parse the preprocessed header
  auto index = clang_createIndex(0, 0);
  if (index == nullptr) {
    std::println(stderr, "failed to create index");
    exit(EXIT_FAILURE);
  }
  scoped_deleter index_deleter([index]() { clang_disposeIndex(index); });
  auto           unit = clang_parseTranslationUnit(
    index,
    input.c_str(),
    nullptr,
    0,
    nullptr,
    0,
    CXTranslationUnit_DetailedPreprocessingRecord | CXTranslationUnit_SkipFunctionBodies
  );

  if (unit == nullptr) {
    std::println(stderr, "failed to parse translation unit");
    exit(EXIT_FAILURE);
  }
  scoped_deleter unit_deleter([unit]() { clang_disposeTranslationUnit(unit); });

  clang_visitChildren(clang_getTranslationUnitCursor(unit), visitor, &data);
  return 0;
}