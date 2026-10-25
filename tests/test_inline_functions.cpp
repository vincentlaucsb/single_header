#include "test_support.hpp"
#include <catch2/generators/catch_generators.hpp>

namespace {
struct InlineFixture {
  fs::path work, include, output;
  explicit InlineFixture(const std::string &name)
      : work(output_dir() / name), include(work / "include"), output(work / "generated.hpp") {
    fs::remove_all(work);
    fs::create_directories(include);
  }
  void files(const std::string &header, const std::string &source) const {
    write_text(include / "api.hpp", "#pragma once\n" + header);
    write_text(include / "api.cpp", "#include \"api.hpp\"\n" + source);
  }
  std::string command(const std::string &extra = {}) const {
    return quote(tool_path()) + " --include-root " + quote(include) + " --output " +
        quote(output) + " " + extra;
  }
};
}

TEST_CASE("function inline JSON and CLI overrides are explicit", "[inline-functions][config]") {
  InlineFixture fixture("inline_options");
  fixture.files("int value();\n", "int value() { return 7; }\n");
  const auto config = fixture.work / "settings.json";
  nlohmann::json settings = {{"includeRoots", {fixture.include.generic_string()}},
      {"output", fixture.output.generic_string()}, {"inlineFunctions", true},
      {"frontendArgs", {"-std=c++11"}}};
  write_text(config, settings.dump());
  const auto command = quote(tool_path()) + " --config " + quote(config);
  run_command(command + " --no-inline-functions");
  CHECK(read_text(fixture.output).find("inline int value()") == std::string::npos);
#ifdef SINGLE_HEADER_TEST_WITH_CLANG
  run_command(command);
  CHECK(read_text(fixture.output).find("inline int value()") != std::string::npos);
#else
  const auto unavailable = run_result(command);
  CHECK(unavailable.status != 0);
  CHECK(unavailable.error.find("SINGLE_HEADER_WITH_CLANG=ON") != std::string::npos);
#endif
  settings["inlineFunctions"] = "true";
  write_text(config, settings.dump());
  const auto invalid = run_result(command);
  CHECK(invalid.status != 0);
  CHECK(invalid.error.find("inlineFunctions") != std::string::npos);
  CHECK(invalid.error.find("boolean") != std::string::npos);
}

#ifdef SINGLE_HEADER_TEST_WITH_CLANG
TEST_CASE("generator inline definitions preserve two-TU identity and local statics",
          "[inline-functions][runtime]") {
  const auto mode = GENERATE(std::string("off"), std::string("all"));
  const bool exceptions = GENERATE(false, true);
  InlineFixture fixture("inline_runtime_" + mode + (exceptions ? "_exceptions" : "_noexceptions"));
  const std::string header = R"(
int bump();
struct Box { Box(); virtual ~Box(); virtual int value(); };
template<class T> T identity(T value) { return value; }
template<> int identity<int>(int value);
constexpr int constant() { return 4; }
inline int existing() { return 3; }
[[noreturn]] void stop();
void removed() = delete;
)";
  const std::string source = R"(
int bump() { static int value = 0; return ++value; }
Box::Box() = default;
Box::~Box() = default;
int Box::value() { return 8; }
template<> int identity<int>(int value) { return value + 1; }
[[noreturn]] void stop() { for (;;) {} }
static int internal() { return 2; }
namespace { int private_value() { return 1; } }
)" + std::string(exceptions ? "int guarded() try { return 5; } catch (...) { return 0; }\n" : "");
  fixture.files(header, source);
  const auto original = read_text(fixture.include / "api.cpp");
  const auto standard = mode == "off" ? "-std=c++11" : "-std=c++17";
  const auto report = fixture.work / "report.json";
  run_command(fixture.command("--inline-functions --function-stitching " + mode +
      " --frontend-arg " + standard + (exceptions ? "" : " --frontend-arg -fno-exceptions") +
      " --stitching-report " + quote(report)));
  const auto analysis = nlohmann::json::parse(read_text(report));
  // Counts original definitions receiving an insertion, including bodies later
  // relocated by stitching; the same fixture has the same count in either mode.
  CHECK(analysis.at("counts").at("inlineInsertions") == (exceptions ? 7 : 6));
  CHECK(read_text(fixture.include / "api.cpp") == original);
  const auto generated = read_text(fixture.output);
  CHECK(generated.find("inline inline") == std::string::npos);
  CHECK(generated.find("inline constexpr") == std::string::npos);
  CHECK(generated.find("inline static int internal") == std::string::npos);
  CHECK(generated.find("inline int Box::value()") != std::string::npos);
  CHECK(generated.find("inline Box::~Box() = default;") != std::string::npos);
  CHECK(generated.find("template<> inline int identity<int>") != std::string::npos);
  const auto project = fixture.work / "consumer";
  write_text(project / "one.cpp", "#include \"generated.hpp\"\n"
      "int (*one_address())() { return &bump; }\nint one() { return bump(); }\n");
  write_text(project / "two.cpp", "#include \"generated.hpp\"\n"
      "int (*two_address())() { return &bump; }\nint two() { Box box; return bump() + box.value() + identity(2) + constant() + existing(); }\n");
  write_text(project / "main.cpp", "int (*one_address())(); int (*two_address())(); int one(); int two();\n"
      "int main() { return one_address() == two_address() && one() == 1 && two() == 20 ? 0 : 1; }\n");
  write_text(project / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\nproject(inline_consumer LANGUAGES CXX)\n"
      "add_executable(consumer one.cpp two.cpp main.cpp)\nset_target_properties(consumer PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED YES)\n"
      "target_include_directories(consumer PRIVATE \"" + fixture.work.generic_string() + "\")\n"
      "enable_testing()\nadd_test(NAME runtime COMMAND consumer)\n");
  run_command(quote(cmake_command()) + " -S " + quote(project) + " -B " + quote(fixture.work / "build") +
      " -DCMAKE_BUILD_TYPE=Release -G " + quote_value(cmake_generator()));
  run_command(quote(cmake_command()) + " --build " + quote(fixture.work / "build") + " --config Release");
  run_command(quote(cmake_command()) + " -E chdir " + quote(fixture.work / "build") + " ctest -C Release --output-on-failure");
}

TEST_CASE("inlining covers conditional source definitions across explicit contexts",
          "[inline-functions][contexts]") {
  InlineFixture fixture("inline_contexts");
  fixture.files("int platform();\nint choice();\n", R"(
#ifdef SECOND_PLATFORM
int platform() { return 2; }
#else
int platform() { return 1; }
#endif
int choice() {
#ifdef SECOND_PLATFORM
return 2;
#else
return 1;
#endif
}
)");
  write_text(fixture.output, "previous output");
  const auto missing = run_result(fixture.command("--inline-functions --frontend-arg -std=c++11"));
  CHECK(missing.status != 0);
  CHECK(missing.error.find("unobserved C++ tokens") != std::string::npos);
  CHECK(read_text(fixture.output) == "previous output");
  nlohmann::json database = nlohmann::json::array();
  for (const auto &define : {std::string(""), std::string("-DSECOND_PLATFORM")}) {
    std::vector<std::string> args{"clang++", "-std=c++11"};
    if (!define.empty()) args.push_back(define);
    args.push_back((fixture.include / "api.cpp").generic_string());
    database.push_back({{"directory", fs::current_path().generic_string()},
        {"file", (fixture.include / "api.cpp").generic_string()}, {"arguments", args}});
  }
  const auto path = fixture.work / "compile_commands.json";
  write_text(path, database.dump());
  run_command(fixture.command("--inline-functions --compilation-database " + quote(path)));
  const auto generated = read_text(fixture.output);
  CHECK(generated.find("inline int platform() { return 2;") != std::string::npos);
  CHECK(generated.find("inline int platform() { return 1;") != std::string::npos);
  CHECK(generated.find("inline int choice()") != std::string::npos);
}

TEST_CASE("inline-only headers and variable ownership remain separate",
          "[inline-functions]") {
  InlineFixture fixture("inline_header_only");
  write_text(fixture.include / "api.hpp", "#pragma once\nint value() { return 7; }\n");
  run_command(fixture.command("--inline-functions --frontend-arg -std=c++11"));
  CHECK(read_text(fixture.output).find("inline int value()") != std::string::npos);
  fixture.files("extern int state;\nstruct State { static int count; };\nint value();\n",
      "int state = 7;\nint State::count = 2;\nint value() { return state; }\n");
  run_command(fixture.command("--inline-functions --frontend-arg -std=c++11"));
  const auto generated = read_text(fixture.output);
  CHECK(generated.find("inline int value()") != std::string::npos);
  CHECK(generated.find("inline int state") == std::string::npos);
  CHECK(generated.find("inline int State::count") == std::string::npos);
}

TEST_CASE("inline requests reject missing contexts and macro-generated declarations",
          "[inline-functions][errors]") {
  InlineFixture fixture("inline_errors");
  fixture.files("int value();\n", "#define FUNCTION(name) int name() { return 1; }\nFUNCTION(value)\n");
  auto result = run_result(fixture.command("--inline-functions"));
  CHECK(result.status != 0);
  CHECK(result.error.find("exactly one") != std::string::npos);
  result = run_result(fixture.command("--inline-functions --frontend-arg -std=c++11"));
  CHECK(result.status != 0);
  CHECK(result.error.find("macro-generated declaration") != std::string::npos);
  result = run_result(fixture.command("--inline-functions --frontend-arg -std=c++invalid"));
  CHECK(result.status != 0);
  CHECK(result.error.find("original analysis failed") != std::string::npos);
  fixture.files("int value();\n", "#define FUNCTION(name) int name() { return 1; }\n"
      "#ifdef HIDDEN\nFUNCTION(value)\n#endif\n");
  result = run_result(fixture.command("--inline-functions --frontend-arg -std=c++11"));
  CHECK(result.status != 0);
  CHECK(result.error.find("unobserved C++ tokens") != std::string::npos);
}

TEST_CASE("source explicit template instantiation definitions require one owner",
          "[inline-functions][templates]") {
  InlineFixture fixture("inline_instantiations");
  const std::string function_template = "template<class T> T identity(T value) { return value; }\n";
  fixture.files(function_template, "template int identity<int>(int);\n");
  auto result = run_result(fixture.command("--inline-functions --frontend-arg -std=c++11"));
  CHECK(result.status != 0);
  CHECK(result.error.find("explicit instantiation definition") != std::string::npos);
  fixture.files("template<class T> struct Holder { T value() { return T(); } };\n",
      "template class Holder<int>;\n");
  result = run_result(fixture.command("--inline-functions --frontend-arg -std=c++11"));
  CHECK(result.status != 0);
  CHECK(result.error.find("explicit instantiation definition") != std::string::npos);
  fixture.files(function_template, "extern template int identity<int>(int);\n");
  run_command(fixture.command("--inline-functions --frontend-arg -std=c++11"));
}

TEST_CASE("attributed constructors preserve a physical qualified insertion point",
          "[inline-functions][attributes]") {
  InlineFixture fixture("inline_attributed_constructor");
  fixture.files("#define NONNULL(...) __attribute__((nonnull(__VA_ARGS__)))\n"
      "struct Box { explicit Box(const int *); };\n",
      "NONNULL(2)\nBox::Box(const int *value) { (void)value; }\n");
  run_command(fixture.command("--inline-functions --frontend-arg -std=c++11"));
  CHECK(read_text(fixture.output).find("NONNULL(2)\ninline Box::Box") != std::string::npos);
}

TEST_CASE("empty source declaration macros cannot hide conditional definitions",
          "[inline-functions][coverage]") {
  InlineFixture fixture("inline_empty_declaration_macro");
  fixture.files("#ifdef HIDDEN\n#define DECLARE int hidden() { return 1; }\n"
      "#else\n#define DECLARE\n#endif\nint value();\n",
      "DECLARE\nint value() { return 2; }\n");
  const auto result = run_result(fixture.command("--inline-functions --frontend-arg -std=c++11"));
  CHECK(result.status != 0);
  CHECK(result.error.find("source macro outside an observed function") != std::string::npos);
  fixture.files("#define BODY\nint value();\n", "int value() { BODY return 2; }\n");
  run_command(fixture.command("--inline-functions --frontend-arg -std=c++11"));
}
#endif
