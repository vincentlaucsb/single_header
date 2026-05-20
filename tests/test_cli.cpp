#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path tool_path() { return SINGLE_HEADER_TEST_TOOL; }
fs::path fixture_dir() { return SINGLE_HEADER_TEST_FIXTURES; }
fs::path output_dir() { return SINGLE_HEADER_TEST_OUTPUT_DIR; }
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

void run_command(const std::string &command) {
  INFO(command);
  static int command_index = 0;
  const auto script_dir = output_dir() / "scripts";
  fs::create_directories(script_dir);

#ifdef _WIN32
  const auto script = script_dir / ("command_" + std::to_string(command_index++) + ".cmd");
  write_text(script, "@echo off\n" + command + "\n");
  REQUIRE(std::system(("cmd /S /C " + quote(script)).c_str()) == 0);
#else
  const auto script = script_dir / ("command_" + std::to_string(command_index++) + ".sh");
  write_text(script, "#!/usr/bin/env sh\nset -e\n" + command + "\n");
  REQUIRE(std::system(("sh " + quote(script)).c_str()) == 0);
#endif
}

void run_command_in_dir(const fs::path &working_dir, const std::string &command) {
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

void expect_golden(const std::string &fixture_name, const std::string &extra_args = {}) {
  const auto fixture = fixture_dir() / fixture_name;
  const auto output = output_dir() / (fixture_name + ".hpp");
  fs::create_directories(output.parent_path());

  auto command = quote(tool_path()) +
                 " --include-root " + quote(fixture / "include") +
                 " --output " + quote(output);
  if (!extra_args.empty()) {
    command += " " + extra_args;
  }

  run_command(command);
  CHECK(read_text(output) == read_text(fixture / "expected.hpp"));
}

} // namespace

TEST_CASE("generates golden output for a small fake library", "[golden]") {
  expect_golden("simple");
}

TEST_CASE("generates golden output for csv-parser style fixture", "[golden]") {
  expect_golden("csv_style",
                "--rewrite-macro CSV_INLINE=inline --implementation-marker " +
                  quote_value("/** INSERT_CSV_SOURCES **/"));
}

TEST_CASE("generated header compiles in multiple translation units", "[compile-smoke]") {
  const auto fixture = fixture_dir() / "compile_smoke";
  const auto work = output_dir() / "compile_smoke";
  const auto generated = work / "generated";
  const auto project = work / "project";
  const auto build = work / "build";

  fs::remove_all(work);
  fs::create_directories(generated);
  fs::create_directories(project);

  run_command(quote(tool_path()) +
              " --include-root " + quote(fixture / "include") +
              " --output " + quote(generated / "smoke_single.hpp") +
              " --implementation-marker " + quote_value("SMOKE_IMPLEMENTATION_MARKER"));

  write_text(project / "CMakeLists.txt",
             "cmake_minimum_required(VERSION 3.20)\n"
             "project(smoke_consumer LANGUAGES CXX)\n"
             "add_library(smoke_consumer_1 OBJECT tu1.cpp)\n"
             "add_library(smoke_consumer_2 OBJECT tu2.cpp)\n"
             "target_compile_features(smoke_consumer_1 PRIVATE cxx_std_17)\n"
             "target_compile_features(smoke_consumer_2 PRIVATE cxx_std_17)\n"
             "target_include_directories(smoke_consumer_1 PRIVATE \"" +
               generated.generic_string() + "\")\n"
             "target_include_directories(smoke_consumer_2 PRIVATE \"" +
               generated.generic_string() + "\")\n");

  write_text(project / "tu1.cpp",
             "#include \"smoke_single.hpp\"\n"
             "int smoke_one() {\n"
             "  return smoke::answer() + smoke::generated();\n"
             "}\n");

  write_text(project / "tu2.cpp",
             "#include \"smoke_single.hpp\"\n"
             "int smoke_two() {\n"
             "  smoke::Thing thing{};\n"
             "  return thing.value();\n"
             "}\n");

  auto configure = quote(cmake_command()) +
                   " -S " + quote(project) +
                   " -B " + quote(build) +
                   " -DCMAKE_BUILD_TYPE=Release";
  if (!cmake_generator().empty()) {
    configure += " -G " + quote_value(cmake_generator());
  }

  run_command(configure);
  run_command(quote(cmake_command()) + " --build " + quote(build) + " --config Release");
}

TEST_CASE("loads single_header.json from the working directory by convention", "[config]") {
  const auto fixture = fixture_dir() / "simple";
  const auto work = output_dir() / "config_convention";
  fs::remove_all(work);
  fs::create_directories(work);

  write_text(work / "single_header.json",
             "{\n"
             "  \"includeRoots\": [" + json_string(fs::relative(fixture / "include", work)) + "],\n"
             "  \"output\": \"generated/simple.hpp\"\n"
             "}\n");

  run_command_in_dir(work, quote(tool_path()));

  CHECK(read_text(work / "generated" / "simple.hpp") ==
        read_text(fixture / "expected.hpp"));
}

TEST_CASE("explicit CLI flags override JSON config values", "[config]") {
  const auto fixture = fixture_dir() / "csv_style";
  const auto work = output_dir() / "config_override";
  const auto config_output = work / "generated" / "from_config.hpp";
  const auto override_output = work / "generated" / "from_cli.hpp";
  fs::remove_all(work);
  fs::create_directories(work);

  const auto config = work / "custom.json";
  write_text(config,
             "{\n"
             "  \"includeRoots\": [" + json_string(fixture / "include") + "],\n"
             "  \"output\": " + json_string(config_output) + ",\n"
             "  \"rewriteMacros\": {\n"
             "    \"CSV_INLINE\": \"wrong\"\n"
             "  },\n"
             "  \"implementationMarker\": \"NEVER_MATCH\"\n"
             "}\n");

  run_command(quote(tool_path()) +
              " --config " + quote(config) +
              " --output " + quote(override_output) +
              " --rewrite-macro CSV_INLINE=inline" +
              " --implementation-marker " + json_string_value("/** INSERT_CSV_SOURCES **/"));

  CHECK_FALSE(fs::exists(config_output));
  CHECK(read_text(override_output) == read_text(fixture / "expected.hpp"));
}
