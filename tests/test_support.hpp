#pragma once
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

fs::path tool_path() { return SINGLE_HEADER_TEST_TOOL; }
fs::path fixture_dir() { return SINGLE_HEADER_TEST_FIXTURES; }
fs::path output_dir() {
#ifdef _WIN32
  const auto pid = _getpid();
#else
  const auto pid = getpid();
#endif
  return fs::path(SINGLE_HEADER_TEST_OUTPUT_DIR) / std::to_string(pid);
}
fs::path cmake_command() { return SINGLE_HEADER_TEST_CMAKE_COMMAND; }
std::string cmake_generator() { return SINGLE_HEADER_TEST_CMAKE_GENERATOR; }

std::string quote(const fs::path &path) {
  auto value = path.string();
  std::string quoted = "\"";
  for (char ch : value) {
    if (ch == '"') {
      quoted += "\\\"";
    } else {
      quoted += ch;
    }
  }
  quoted += "\"";
  return quoted;
}

std::string quote_value(const std::string &value) {
  std::string quoted = "\"";
  for (char ch : value) {
    if (ch == '"') {
      quoted += "\\\"";
    } else {
      quoted += ch;
    }
  }
  quoted += "\"";
  return quoted;
}

std::string json_string(const fs::path &path) {
  return quote_value(path.generic_string());
}

std::string json_string_value(const std::string &value) {
  return quote_value(value);
}

void write_text(const fs::path &path, const std::string &text);

struct CommandResult {
  int status;
  std::string out, error;
};
std::string read_text(const fs::path &path);

CommandResult run_result(const std::string &command) {
  INFO(command);
  static int command_index = 0;
  const auto command_name =
      "command_" +
      std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()) +
      "_" + std::to_string(command_index++);
  const auto script_dir = output_dir() / "scripts";
  fs::create_directories(script_dir);

#ifdef _WIN32
  const auto script = script_dir / (command_name + ".cmd");
  write_text(script, "@echo off\n" + command + "\n");
  const auto process = "cmd /S /C \"" + script.generic_string() + "\"";
#else
  const auto script = script_dir / (command_name + ".sh");
  write_text(script, "#!/usr/bin/env sh\nset -e\n" + command + "\n");
  const auto process = "sh \"" + script.generic_string() + "\"";
#endif
  const auto wrapper = fs::path(script.string() + ".cmake");
  const auto stdout_path = fs::path(script.string() + ".stdout");
  const auto stderr_path = fs::path(script.string() + ".stderr");
  const auto status_path = fs::path(script.string() + ".status");
  write_text(wrapper, "execute_process(COMMAND " + process +
                          " TIMEOUT 180 RESULT_VARIABLE result "
                          "OUTPUT_FILE \"" +
                          stdout_path.generic_string() + "\" ERROR_FILE \"" +
                          stderr_path.generic_string() +
                          "\")\n"
                          "file(WRITE \"" +
                          status_path.generic_string() + "\" \"${result}\")\n");
  auto launch = quote(cmake_command()) + " -P " + quote(wrapper);
#ifdef _WIN32
  launch = "\"" + launch + "\"";
#endif
  REQUIRE(std::system(launch.c_str()) == 0);
  const auto status = read_text(status_path);
  int code = -1;
  try {
    code = std::stoi(status);
  } catch (const std::exception &) {
  }
  return {code, read_text(stdout_path), read_text(stderr_path)};
}

void run_command(const std::string &command) {
  const auto result = run_result(command);
  INFO(result.out);
  INFO(result.error);
  REQUIRE(result.status == 0);
}

void run_command_in_dir(const fs::path &working_dir,
                        const std::string &command) {
#ifdef _WIN32
  run_command("cd /D " + quote(working_dir) + " && " + command);
#else
  run_command("cd " + quote(working_dir) + " && " + command);
#endif
}

std::string read_text(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to read " + path.string());
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

void write_text(const fs::path &path, const std::string &text) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("failed to write " + path.string());
  }
  output << text;
}

void expect_golden(const std::string &fixture_name,
                   const std::string &extra_args = {}) {
  const auto fixture = fixture_dir() / fixture_name;
  const auto output = output_dir() / (fixture_name + ".hpp");
  fs::create_directories(output.parent_path());

  auto command = quote(tool_path()) + " --include-root " +
                 quote(fixture / "include") + " --output " + quote(output);
  if (!extra_args.empty()) {
    command += " " + extra_args;
  }

  run_command(command);
  CHECK(read_text(output) == read_text(fixture / "expected.hpp"));
}

} // namespace
