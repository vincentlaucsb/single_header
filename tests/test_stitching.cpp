#include "test_support.hpp"

namespace {
struct StitchFixture {
  fs::path work, include, output, report;
  explicit StitchFixture(const std::string &name)
      : work(output_dir() / name), include(work / "include"),
        output(work / "generated.hpp"), report(work / "report.json") {
    fs::remove_all(work);
    fs::create_directories(include);
  }
  void files(const std::string &header, const std::string &source) {
    write_text(include / "api.hpp", header);
    write_text(include / "api.cpp", "#include \"api.hpp\"\n" + source);
  }
  std::string command(const std::string &mode = "all",
                      const std::string &extra = {}) const {
    return quote(tool_path()) + " --include-root " + quote(include) +
           " --output " + quote(output) + " --function-stitching " + mode +
           " --frontend-arg -std=c++17 --stitching-report " + quote(report) +
           " " + extra;
  }
  nlohmann::json generate(const std::string &mode = "all",
                          const std::string &extra = {}) const {
    run_command(command(mode, extra));
    return nlohmann::json::parse(read_text(report));
  }
  std::string reason(const nlohmann::json &data,
                     const std::string &name) const {
    for (const auto &candidate : data.at("candidates"))
      if (candidate.at("name") == name)
        return candidate.at("reason");
    return "missing candidate";
  }
  void runtime(const std::string &one, const std::string &two,
               const std::string &main,
               const std::string &preamble = {},
               const std::string &extra_cmake = {}) const {
    const auto project = work / "consumer";
    write_text(project / "one.cpp",
               preamble + "#include \"generated.hpp\"\n" + one);
    write_text(project / "two.cpp", "#include \"generated.hpp\"\n" + two);
    write_text(project / "main.cpp", main);
    write_text(
        project / "CMakeLists.txt",
        "cmake_minimum_required(VERSION 3.20)\nproject(stitched LANGUAGES "
        "CXX)\n"
        "add_executable(consumer one.cpp two.cpp "
        "main.cpp)\ntarget_compile_features(consumer PRIVATE cxx_std_17)\n" + extra_cmake +
        "target_include_directories(consumer PRIVATE \"" +
            work.generic_string() +
            "\")\n"
            "enable_testing()\nadd_test(NAME runtime COMMAND consumer)\n");
    auto configure = quote(cmake_command()) + " -S " + quote(project) + " -B " +
                     quote(work / "build") + " -DCMAKE_BUILD_TYPE=Release -G " +
                     quote_value(cmake_generator());
    run_command(configure);
    run_command(quote(cmake_command()) + " --build " + quote(work / "build") +
                " --config Release");
    run_command(quote(cmake_command()) + " -E chdir " + quote(work / "build") +
                " ctest -C Release --output-on-failure");
  }
};
} // namespace

TEST_CASE("config-only generation covers source variants from another working directory",
          "[stitching][config][frontend]") {
  StitchFixture fixture("config_only_variants");
  fixture.files("#pragma once\n#include <detail.hpp>\nint base();\n"
                "#if EXTRA\nint alternate();\n#endif\nint other();\n",
                "int base() { return configured_value; }\n"
                "#if EXTRA\nint alternate() { return 2; }\n#endif\n");
  write_text(fixture.include / "detail.hpp", "#pragma once\nconstexpr int configured_value = 40;\n");
  write_text(fixture.include / "other.cpp", "#include \"api.hpp\"\nint other() { return base(); }\n");
  const auto config = fixture.work / "single_header.json";
  nlohmann::json settings{{"includeRoots", {"include"}}, {"entry", "include/api.hpp"},
      {"output", "generated.hpp"}, {"stitchingReport", "report.json"},
      {"inlineFunctions", true}, {"functionStitching", "all"},
      {"frontendArgs", {"-std=c++17", "-Iinclude"}},
      {"frontendVariants", {{{"sources", {"include/api.cpp"}}, {"args", {"-DEXTRA=1"}}}}}};
  write_text(config, settings.dump());
  const auto command = quote(tool_path()) + " --config " + quote(config);
  run_command(command);
  const auto report = nlohmann::json::parse(read_text(fixture.report));
  CHECK(report.at("configurations").size() == 3);
  size_t variants = 0;
  for (const auto &context : report.at("configurations")) {
    const auto args = context.at("arguments").get<std::vector<std::string>>();
    if (std::find(args.begin(), args.end(), "-DEXTRA=1") != args.end()) {
      ++variants;
      CHECK(fs::path(context.at("source").get<std::string>()).filename() == "api.cpp");
    }
  }
  CHECK(variants == 1);
  fixture.runtime("int one() { return base() + alternate(); }\n",
                  "int two() { return other(); }\n",
                  "int one(); int two(); int main() { return one() == 42 && two() == 40 ? 0 : 1; }\n",
                  "#define EXTRA 1\n");
  const auto cli_include = quote_value("-I" + fs::relative(fixture.include, fs::current_path()).generic_string());
  const auto cli_context = command + " --frontend-arg -std=c++17 --frontend-arg " + cli_include;
  run_command(cli_context + " --frontend-arg -DEXTRA=1");
  CHECK(nlohmann::json::parse(read_text(fixture.report)).at("configurations").size() == 2);
  const auto override = run_result(cli_context);
  CHECK(override.status != 0);
  CHECK(override.error.find("unobserved C++ tokens") != std::string::npos);
  settings["frontendVariants"][0]["sources"] = {"include/other.cpp"};
  write_text(config, settings.dump());
  const auto unmatched = run_result(command);
  CHECK(unmatched.status != 0);
  CHECK(unmatched.error.find("unobserved C++ tokens") != std::string::npos);
}

TEST_CASE("stitches real C++ library bodies and preserves multi-TU identity "
          "and local state",
          "[stitching][runtime]") {
  StitchFixture fixture("stitch_runtime");
  fixture.files(R"(#ifndef DEMO_API_HPP
#define DEMO_API_HPP
#include <string>
#include <vector>
namespace demo {
using Count = int;
std::string label(int = 2);
int bump();
int recursive(int value);
int mutual_b(int);
int mutual_a(int);
int mutual_b(int);
int mutual_a(int);
struct Box {
  int sum(int header = 3) const & noexcept;
  static int twice(int);
  void replace(const std::vector<int>&);
  int later() const;
  int value = 7;
  std::vector<int> rows;
};
}
#endif
)",
                R"(namespace demo {
std::string label(int count) { return std::string("rows:") + std::to_string(count); }
int bump() { static int value = 0; return ++value; }
int recursive(int count) { if (count == 0) return 1; return count * recursive(count - 1); }
int mutual_b(int n) { return n == 0 ? 0 : mutual_a(n - 1); }
int mutual_a(int n) { return n == 0 ? 1 : mutual_b(n - 1); }
int Box::sum(int input) const & noexcept { return later() + input; }
int Box::later() const { return value; }
int Box::twice(int input) { int result = 0; for (int i = 0; i < input; ++i) result += 2; return result; }
void Box::replace(const std::vector<int> &input) { rows = input; }
}
)");
  const auto data = fixture.generate();
  CHECK(data.at("baseline") == "valid");
  CHECK(data.at("counts").at("acceptedFree") == 5);
  CHECK(data.at("counts").at("acceptedMethods") == 4);
  CHECK(data.at("counts").at("skipped") == 0);
  const auto text = read_text(fixture.output);
  CHECK(text.find("int sum(int input = 3) const & noexcept {") !=
        std::string::npos);
  CHECK(text.find("Box::sum") == std::string::npos);
  fixture.runtime(R"(
const void *one_address() { return reinterpret_cast<const void *>(&demo::bump); }
int one() { demo::Box box; box.replace({1,2,3}); return demo::bump() + box.sum() + box.twice(3) + static_cast<int>(box.rows.size()); }
)",
                  R"(
const void *two_address() { return reinterpret_cast<const void *>(&demo::bump); }
int two() { auto method = &demo::Box::sum; demo::Box box; return demo::bump() + (box.*method)(3); }
bool label_ok() { return demo::label() == "rows:2" && demo::recursive(5) == 120 && demo::mutual_a(4) == 1; }
)",
                  R"(
const void *one_address(); const void *two_address(); int one(); int two(); bool label_ok();
int main() { return one_address() == two_address() && one() == 20 && two() == 12 && label_ok() ? 0 : 1; }
)");
}

TEST_CASE(
    "rejects changed overload selection although tentative output compiles",
    "[stitching][lookup]") {
  StitchFixture fixture("changed_overload");
  fixture.files("#pragma once\nint pick(long);\nint run();\nint pick(int);\n",
                "int pick(long) { return 1; }\nint pick(int) { return 2; "
                "}\nint run() { return pick(0); }\n");
  const auto data = fixture.generate("free");
  CHECK(fixture.reason(data, "run").find("changed_semantics:") == 0);
  CHECK(data.at("counts").at("acceptedFree") == 2);
  const auto text = read_text(fixture.output);
  CHECK(text.find("int run();") != std::string::npos);
  CHECK(text.find("int run() { return pick(0); }") != std::string::npos);
}

TEST_CASE("independent modes select free functions and ordinary methods",
          "[stitching][modes]") {
  StitchFixture fixture("modes");
  fixture.files("#pragma once\ninline int f();\nstruct Box { int value(); };\n",
                "int f() { return 4; }\nint Box::value() { return 5; }\n");
  auto free = fixture.generate("free");
  CHECK(free.at("counts").at("acceptedFree") == 1);
  CHECK(free.at("counts").at("acceptedMethods") == 0);
  CHECK(read_text(fixture.output).find("inline inline") == std::string::npos);
  auto methods = fixture.generate("methods");
  CHECK(methods.at("counts").at("acceptedFree") == 0);
  CHECK(methods.at("counts").at("acceptedMethods") == 1);
}

TEST_CASE(
    "retains unsupported and dependency-bound definitions transactionally",
    "[stitching][fallback]") {
  StitchFixture fixture("fallback");
  fixture.files(R"(#pragma once
int needs_source();
int configured();
int macro_body();
int conditional_body();
struct Box { virtual int virtual_method(); };
template<class T> T generic(T value);
)",
                R"(#include <vector>
int needs_source() { std::vector<int> values{1,2,3}; return static_cast<int>(values.size()); }
#if 1
int configured() { return 1; }
#endif
#define ANSWER 42
int macro_body() { return ANSWER; }
int conditional_body() {
#if 1
  return 1;
#else
  return 2;
#endif
}
int Box::virtual_method() { return 1; }
template<class T> T generic(T value) { return value; }
)");
  const auto data = fixture.generate();
  CHECK(fixture.reason(data, "needs_source") == "destination_parse_error");
  CHECK(fixture.reason(data, "configured") == "conditional_region");
  CHECK(fixture.reason(data, "conditional_body") == "accepted");
  CHECK(fixture.reason(data, "macro_body") == "destination_parse_error");
  CHECK(fixture.reason(data, "Box::virtual_method") == "virtual_method");
  CHECK(fixture.reason(data, "generic") == "template");
  CHECK(data.at("counts").at("removedBodies") == 1);
  const auto stitched = read_text(fixture.output);
  run_command(fixture.command("off"));
  CHECK(read_text(fixture.output) != stitched);
}

TEST_CASE("physical lexer distinguishes guards from feature regions and fake "
          "directives",
          "[stitching][preprocessor]") {
  StitchFixture fixture("physical_lexer");
  fixture.files("#pragma once\nint raw();\nint feature();\n", R"(
int preceding = 0;
#ifndef FEATURE_DISABLED
#define FEATURE_DISABLED
int feature() { return 2; }
#endif
int raw() { const char *text = R"raw(
#if 0
#pragma pack(1)
)raw"; /* #if 0 */ return text[0] == '\n' ? 1 : 0; }
)");
  const auto data = fixture.generate();
  CHECK(fixture.reason(data, "feature") == "conditional_region");
  CHECK(fixture.reason(data, "raw") == "accepted");
}

TEST_CASE("configured empty and inline annotations remove their complete "
          "source spelling",
          "[stitching][annotations]") {
  StitchFixture fixture("annotation");
  fixture.files(
      "#pragma once\nstruct Box { int value(); };\n",
      "#define CSV_INLINE\nCSV_INLINE int Box::value() { return 3; }\n");
  auto skipped = fixture.generate();
  CHECK(fixture.reason(skipped, "Box::value") == "signature_macro");
  auto accepted = fixture.generate(
      "methods",
      "--inline-annotation CSV_INLINE --rewrite-macro CSV_INLINE=inline");
  CHECK(fixture.reason(accepted, "Box::value") == "accepted");
  CHECK(read_text(fixture.output).find("CSV_INLINE int Box::value") ==
        std::string::npos);
  fixture.files(
      "#pragma once\nstruct Box { int value(); };\n",
      "#define CSV_INLINE inline\nCSV_INLINE int Box::value() { return 3; }\n");
  accepted = fixture.generate("methods", "--inline-annotation CSV_INLINE");
  CHECK(fixture.reason(accepted, "Box::value") == "accepted");
}

TEST_CASE("missing, ambiguous, and relative compilation database contexts are "
          "explicit",
          "[stitching][contexts]") {
  StitchFixture fixture("database with spaces");
  fixture.files("#pragma once\nint value();\n",
                "#if CONTEXT_VALUE != 7\n#error wrong context\n#endif\nint "
                "value() { return 7; }\n");
  const auto database = fixture.work / "compile_commands.json";
  const auto basic =
      quote(tool_path()) + " --include-root " + quote(fixture.include) +
      " --output " + quote(fixture.output) +
      " --function-stitching free --compilation-database " + quote(database);
  write_text(database, "[]");
  auto missing = run_result(basic);
  CHECK(missing.status != 0);
  CHECK(missing.error.find("found 0") != std::string::npos);
  const auto command =
      nlohmann::json{{"directory", fixture.work.generic_string()},
                     {"file", "include/api.cpp"},
                     {"arguments",
                      {"clang++", "-std=c++17", "-DCONTEXT_VALUE=7", "-c",
                       "include/api.cpp"}}};
  write_text(database, nlohmann::json::array({command, command}).dump());
  auto ambiguous = run_result(basic);
  CHECK(ambiguous.status != 0);
  CHECK(ambiguous.error.find("found 2") != std::string::npos);
  write_text(database, nlohmann::json::array({command}).dump());
  run_command(basic);
  CHECK(read_text(fixture.output).find("inline int value()") !=
        std::string::npos);
}

TEST_CASE("reports cannot alias inputs or output and never contaminate "
          "generated stdout",
          "[stitching][report]") {
  StitchFixture fixture("reports");
  fixture.files("#pragma once\nint value();\n", "int value() { return 7; }\n");
  auto collision = run_result(
      fixture.command("all", "--stitching-report " + quote(fixture.output)));
  CHECK(collision.status != 0);
  collision = run_result(fixture.command(
      "all", "--stitching-report " + quote(fixture.include / "api.cpp")));
  CHECK(collision.status != 0);
  const auto result = run_result(fixture.command("all", "--output -"));
  REQUIRE(result.status == 0);
  CHECK(result.out.find("#pragma once") == 0);
  CHECK(result.out.find("stitching accepted") == std::string::npos);
  CHECK(result.error.find("accepted 1/1") != std::string::npos);
}

TEST_CASE("unchanged macro-prefixed header constants retain original "
          "declaration identities",
          "[stitching][origins]") {
  StitchFixture fixture("origins");
  fixture.files(R"(#pragma once
#define CONSTANT constexpr
namespace constants {
CONSTANT char message[] = "unchanged";
inline int length() { return sizeof(message); }
int safe();
int uses_internal();
}
)",
                R"(namespace constants {
int safe() { return length(); }
int uses_internal() { return message[0]; }
}
)");
  const auto data = fixture.generate();
  CHECK(data.at("baseline") == "valid");
  CHECK(fixture.reason(data, "constants::safe") == "accepted");
  CHECK(fixture.reason(data, "constants::uses_internal") ==
        "internal_dependency");
}

TEST_CASE("reports retain source state even when every function is stitched",
          "[stitching][report]") {
  StitchFixture fixture("retained_state");
  fixture.files("#pragma once\nextern int state;\nint value();\n",
                "int state = 4;\nint value() { return 7; }\n");
  auto data = fixture.generate();
  CHECK(data.at("counts").at("removedBodies") == 1);
  CHECK(data.at("retainsImplementation") == true);
  fixture.files("#pragma once\nextern int state;\n", "int state = 4;\n");
  data = fixture.generate();
  CHECK(data.at("counts").at("candidates") == 0);
  CHECK(data.at("retainsImplementation") == true);
}

TEST_CASE(
    "mixed fallback keeps a single implementation TU and RAII destruction",
    "[stitching][runtime]") {
  StitchFixture fixture("mixed_fallback");
  fixture.files(R"(#pragma once
namespace mixed {
extern int destructions;
struct Guard { Guard(); ~Guard(); };
int work(int = 5);
int fallback();
}
#ifdef BUILD_IMPLEMENTATION
/* MIXED_IMPLEMENTATION_MARKER */
#endif
)",
                R"(namespace mixed {
int destructions = 0;
static int hidden = 9;
Guard::Guard() {}
Guard::~Guard() { ++destructions; }
int work(int value) { Guard guard; return value * 2; }
int fallback() { return hidden; }
}
)");
  const auto data = fixture.generate(
      "all", "--frontend-arg -DBUILD_IMPLEMENTATION --implementation-marker " +
                 quote_value("/* MIXED_IMPLEMENTATION_MARKER */"));
  CHECK(fixture.reason(data, "mixed::work") == "accepted");
  CHECK(fixture.reason(data, "mixed::fallback") == "internal_dependency");
  CHECK(data.at("retainsImplementation") == true);
  fixture.runtime("int one() { return mixed::work() + mixed::fallback(); }\n",
                  "int two() { const auto result = mixed::work(3); return "
                  "result + mixed::destructions; }\n",
                  "int one(); int two(); int main() { return one() == 19 && "
                  "two() == 8 ? 0 : 1; }\n",
                  "#define BUILD_IMPLEMENTATION\n");
}

TEST_CASE("new destination macros reject a compiling body with changed "
          "expanded tokens",
          "[stitching][preprocessor]") {
  StitchFixture fixture("new_macro");
  fixture.files("#pragma once\n#define local_name other_name\nint value();\n",
                "#undef local_name\nint value() { int local_name = 3; return "
                "local_name; }\n");
  const auto data = fixture.generate();
  CHECK(fixture.reason(data, "value").find("changed_semantics:") == 0);
  CHECK(data.at("counts").at("removedBodies") == 0);
}

TEST_CASE("multiple sources retain namespaces, entry filtering, and "
          "deterministic paired edits",
          "[stitching][contexts]") {
  StitchFixture fixture("multi_source");
  write_text(fixture.include / "api.hpp",
             "#pragma once\n#include \"left.hpp\"\n#include \"right.hpp\"\n");
  write_text(fixture.include / "left.hpp",
             "#pragma once\nnamespace left { using Number = int; Number "
             "same(Number header = 2); }\n");
  write_text(
      fixture.include / "right.hpp",
      "#pragma once\nnamespace right { int same(int); int same(long); }\n");
  write_text(fixture.include / "excluded.hpp",
             "#pragma once\nnamespace excluded { int hidden(); }\n");
  write_text(fixture.include / "left.cpp",
             "#include \"left.hpp\"\nnamespace left { Number same(Number "
             "input) { return input + 1; } }\n");
  write_text(fixture.include / "right.cpp",
             "#include \"right.hpp\"\nnamespace right { int same(int input) { "
             "return input * 2; } int same(long input) { return "
             "static_cast<int>(input) * 3; } }\n");
  write_text(fixture.include / "excluded.cpp",
             "#include \"excluded.hpp\"\nnamespace excluded { int hidden() { "
             "return 7; } }\n");
  const auto extra = "--entry " + quote(fixture.include / "api.hpp");
  auto data = fixture.generate("free", extra);
  CHECK(data.at("counts").at("acceptedFree") == 3);
  CHECK(fixture.reason(data, "excluded::hidden") == "no_emitted_declaration");
  const auto first = read_text(fixture.output);
  CHECK(first.find("Number same(Number input = 2)") != std::string::npos);
  CHECK(first.find("namespace excluded { int hidden();") == std::string::npos);
  data = fixture.generate("free", extra);
  CHECK(read_text(fixture.output) == first);
}

TEST_CASE("lexical editing preserves comments, CRLF input, raw strings, and no "
          "final newline",
          "[stitching][ranges]") {
  StitchFixture fixture("ranges");
  fixture.files("#pragma once\r\n// retained documentation\r\nint "
                "calculate(\r\n int header = 2\r\n);\r\n",
                "// int fake(int);\r\n// source commentary survives "
                "removal\r\nint calculate(int input) {\r\n"
                " const char *text = u8R\"custom({ \\\" fake(); "
                "})custom\";\r\n return input + text[0];\r\n}");
  const auto data = fixture.generate();
  CHECK(fixture.reason(data, "calculate") == "accepted");
  const auto text = read_text(fixture.output);
  CHECK(text.find("// retained documentation") != std::string::npos);
  CHECK(text.find("// source commentary survives removal") !=
        std::string::npos);
  CHECK(text.find("int input = 2") != std::string::npos);
  CHECK(text.find("u8R\"custom({ \\\" fake(); })custom\"") !=
        std::string::npos);
}

TEST_CASE("existing filesystem aliases cannot overwrite an input report",
          "[stitching][report]") {
  StitchFixture fixture("report_aliases");
  fixture.files("#pragma once\nint value();\n", "int value() { return 7; }\n");
  const auto alias = fixture.work / "alias.json";
  std::error_code error;
  fs::create_hard_link(fixture.include / "api.cpp", alias, error);
  if (!error) {
    const auto original = read_text(fixture.include / "api.cpp");
    const auto result = run_result(
        fixture.command("all", "--stitching-report " + quote(alias)));
    CHECK(result.status != 0);
    CHECK(read_text(fixture.include / "api.cpp") == original);
  }
#ifdef _WIN32
  auto upper = (fixture.include / "api.cpp").string();
  std::transform(
      upper.begin(), upper.end(), upper.begin(),
      [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
  CHECK(run_result(fixture.command("all", "--stitching-report " +
                                              quote(fs::path(upper))))
            .status != 0);
#endif
}

TEST_CASE("residual lambdas retain identity without preventing ordinary "
          "neighbors from stitching",
          "[stitching][origins]") {
  StitchFixture fixture("residual_lambda");
  fixture.files("#pragma once\n#define CSV_INLINE\n#include "
                "<algorithm>\n#include <string>\nint "
                "residual();\nint safe();\n",
                "CSV_INLINE int residual() { std::string text(\"ab\"); "
                "std::transform(text.begin(), text.end(), text.begin(), "
                "[](char value) { return static_cast<char>(value + 1); }); "
                "return text[0]; }\nCSV_INLINE int safe() { return 7; }\n");
  const auto data = fixture.generate(
      "all",
      "--inline-annotation CSV_INLINE --rewrite-macro CSV_INLINE=inline");
  CHECK(data.at("baseline") == "valid");
  CHECK(fixture.reason(data, "safe") == "accepted");
  CHECK(fixture.reason(data, "residual") == "accepted");
}

TEST_CASE("structured template arguments preserve closure identity in method "
          "specializations",
          "[stitching][origins]") {
  StitchFixture fixture("lambda_method");
  fixture.files(R"(#pragma once
#include <algorithm>
#include <string>
#include <vector>
#include <cctype>
#define CSV_INLINE
namespace demo {
struct Names { void set_names(const std::vector<std::string>&); std::vector<std::string> names; };
int safe();
}

)",
                R"(namespace demo {
CSV_INLINE void Names::set_names(const std::vector<std::string>& input) {
  names = input;
  for (size_t i = 0; i < input.size(); ++i) {
    std::string lower = input[i];
    std::transform(lower.begin(), lower.end(), lower.begin(),
       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    names[i] = lower;
  }
}
int safe() { return 42; }
}
)");
  const auto data = fixture.generate(
      "all",
      "--inline-annotation CSV_INLINE --rewrite-macro CSV_INLINE=inline");
  CHECK(data.at("baseline") == "valid");
  CHECK(fixture.reason(data, "demo::safe") == "accepted");
  CHECK(fixture.reason(data, "demo::Names::set_names") == "accepted");
}

TEST_CASE("anonymous enums use declaration identity rather than printed source "
          "locations",
          "[stitching][origins]") {
  StitchFixture fixture("anonymous_enum");
  fixture.files(R"(#pragma once
namespace tagged {
enum { count = 3 };
constexpr auto stored = count;
inline int read() { return static_cast<int>(stored); }
int safe();
}
)",
                "namespace tagged { int safe() { return read(); } }\n");
  const auto data = fixture.generate();
  CHECK(data.at("baseline") == "valid");
  CHECK(fixture.reason(data, "tagged::safe") == "accepted");
}

TEST_CASE("non-type template parameters retain depth and index identity in "
          "residual templates",
          "[stitching][origins]") {
  StitchFixture fixture("template_parameters");
  fixture.files(R"(#pragma once
namespace policies {
template<char Mark, bool Enabled> struct Token {
  static int value(char input) { return Enabled && input == Mark ? 1 : 0; }
};
inline int read() { return Token<'+', true>::value('+'); }
int safe();
}
)",
                "namespace policies { int safe() { return read(); } }\n");
  const auto data = fixture.generate();
  CHECK(data.at("baseline") == "valid");
  CHECK(fixture.reason(data, "policies::safe") == "accepted");
}

TEST_CASE(
    "report stream failures are surfaced before generated output is written",
    "[stitching][report]") {
#ifdef _WIN32
  SKIP("Unix /dev/full stream failure injection is unavailable on Windows");
#else
  if (!fs::exists("/dev/full"))
    SKIP("/dev/full is unavailable on this system");
  StitchFixture fixture("report_failure");
  fixture.files("#pragma once\nint value();\n", "int value() { return 7; }\n");
  const auto result =
      run_result(fixture.command("all", "--stitching-report /dev/full"));
  CHECK(result.status != 0);
  CHECK(result.error.find("failed to write") != std::string::npos);
  CHECK_FALSE(fs::exists(fixture.output));
#endif
}

TEST_CASE("local closure members retain their lexical identity before "
          "provenance mapping",
          "[stitching][origins]") {
  StitchFixture fixture("closure_members");
  fixture.files(
      "#pragma once\n#define CSV_INLINE\nint residual();\nint safe();\n",
      "CSV_INLINE int residual() { auto callable = [](int input) { return "
      "input + 1; }; return callable(2); }\n"
      "int safe() { return 7; }\n");
  const auto data = fixture.generate(
      "all",
      "--inline-annotation CSV_INLINE --rewrite-macro CSV_INLINE=inline");
  CHECK(data.at("baseline") == "valid");
  CHECK(fixture.reason(data, "safe") == "accepted");
  CHECK(fixture.reason(data, "residual") == "accepted");
}

TEST_CASE("source-only attribute differences are informational while safe "
          "neighbors move across TUs",
          "[stitching][baseline]") {
  StitchFixture fixture("source_attributes");
  fixture.files(
      "#pragma once\nstruct Box { int method(int *); };\nint safe();\n",
      "__attribute__((nonnull(2))) int Box::method(int *input) { return "
      "*input; }\n");
  write_text(fixture.include / "safe.cpp",
             "#include \"api.hpp\"\nint safe() { return 7; }\n");
  const auto data = fixture.generate();
  CHECK(data.at("baseline") == "valid");
  REQUIRE_FALSE(data.at("baselineDifferences").empty());
  const auto &difference = data.at("baselineDifferences").front();
  CHECK(difference.at("reason").get<std::string>().find(
            "changed_declaration:attributes:") == 0);
  CHECK(difference.at("detail").get<std::string>().find("nonnull") !=
        std::string::npos);
  CHECK(difference.at("severity") == "informational");
  CHECK(fixture.reason(data, "safe") == "accepted");
  CHECK(fixture.reason(data, "Box::method") == "unsupported_attribute");
  CHECK(data.at("counts").at("removedBodies") == 1);
  const auto stitched = read_text(fixture.output);
  CHECK(stitched.find("inline int safe() { return 7; }") != std::string::npos);
  run_command(fixture.command("off"));
  const auto unstitched = read_text(fixture.output);
  CHECK(data.at("bytes").at("unstitched") == unstitched.size());
  CHECK(data.at("bytes").at("stitched") == stitched.size());
  CHECK(data.at("bytes").at("savings") ==
        static_cast<std::int64_t>(unstitched.size()) -
            static_cast<std::int64_t>(stitched.size()));
}

TEST_CASE("preexisting collation differences never hide a changed overload",
          "[stitching][baseline][lookup]") {
  StitchFixture fixture("attribute_and_changed_overload");
  fixture.files(
      "#pragma once\nstruct Box { int method(int *); };\n"
      "int pick(long);\nint run();\nint pick(int);\nint safe();\n",
      "__attribute__((nonnull(2))) int Box::method(int *input) { return "
      "*input; }\nint pick(long) { return 1; }\n"
      "int pick(int) { return 2; }\nint run() { return pick(0); }\n");
  write_text(fixture.include / "safe.cpp",
             "#include \"api.hpp\"\nint safe() { return 7; }\n");
  const auto data = fixture.generate();
  CHECK(data.at("baseline") == "valid");
  CHECK_FALSE(data.at("baselineDifferences").empty());
  CHECK(fixture.reason(data, "safe") == "accepted");
  CHECK(fixture.reason(data, "run").find("changed_semantics:") == 0);
  CHECK(data.at("counts").at("removedBodies") == 3);
  const auto text = read_text(fixture.output);
  CHECK(text.find("int run();") != std::string::npos);
  CHECK(text.find("int run() { return pick(0); }") != std::string::npos);
}

TEST_CASE("an unparseable unstitched artifact blocks relocation explicitly",
          "[stitching][baseline]") {
  StitchFixture fixture("unparseable_baseline");
  fixture.files("#pragma once\nint safe();\n",
                "static int collision = 1;\nint safe() { return 7; }\n");
  // Both original TUs parse, but collation redefines a source-local name.
  write_text(fixture.include / "other.cpp", "static int collision = 2;\n");
  const auto data = fixture.generate();
  CHECK(data.at("baseline") == "invalid");
  CHECK(data.at("baselineReason") == "destination_parse_error");
  CHECK_FALSE(data.at("baselineDiagnostics").get<std::string>().empty());
  CHECK(fixture.reason(data, "safe") == "baseline_invalid");
  CHECK(data.at("counts").at("removedBodies") == 0);
  const auto unchanged = read_text(fixture.output);
  run_command(fixture.command("off"));
  CHECK(read_text(fixture.output) == unchanged);
}

TEST_CASE("all original provenance is collected before generated baselines",
          "[stitching][baseline][origins]") {
  StitchFixture fixture("complete_baseline_provenance");
  fixture.files("#pragma once\nint safe();\nint residual();\n",
                "int safe() { return 7; }\n");
  // This constant is absent from the first original TU. Every generated context
  // sees it, so baselines must wait until discovery has visited the second TU.
  write_text(fixture.include / "z_residual.cpp",
             "#include \"api.hpp\"\nnamespace { const int value = 3; }\n"
             "int residual() { return value; }\n");
  const auto data = fixture.generate();
  CHECK(data.at("baseline") == "valid");
  CHECK(fixture.reason(data, "safe") == "accepted");
  CHECK(fixture.reason(data, "residual") == "internal_dependency");
  CHECK(data.at("counts").at("removedBodies") == 1);
}

TEST_CASE("residual deduced closure return types retain declaration identity",
          "[stitching][baseline][origins]") {
  StitchFixture fixture("closure_return_type");
  fixture.files("#pragma once\nint safe();\n"
                "inline auto factory() { return []() { return 1; }; }\n",
                "int safe() { return 7; }\n");
  const auto data = fixture.generate();
  CHECK(data.at("baseline") == "valid");
  CHECK(fixture.reason(data, "safe") == "accepted");
  CHECK(data.at("counts").at("removedBodies") == 1);
  const auto text = read_text(fixture.output);
  CHECK(text.find("inline int safe() { return 7; }") != std::string::npos);
  CHECK(text.find("inline auto factory() { return []() { return 1; }; }") !=
        std::string::npos);
}

TEST_CASE(
    "moving preprocessing state cannot activate untouched header branches",
    "[stitching][preprocessor]") {
  StitchFixture fixture("body_directive");
  fixture.files("#pragma once\nint value();\n#if NEW_STATE\ninline int "
                "activated() { return 2; }\n#endif\n",
                "int value() {\n#define NEW_STATE 1\nreturn 7;\n}\n");
  const auto data = fixture.generate();
  CHECK(fixture.reason(data, "value") == "body_directive");
  CHECK(data.at("counts").at("removedBodies") == 0);
  const auto original = read_text(fixture.output);
  run_command(fixture.command("off"));
  CHECK(read_text(fixture.output) == original);
}

TEST_CASE("batch validation retains constructor initializer and allocation bindings",
          "[stitching][expanded]") {
  StitchFixture fixture("expanded_bindings");
  fixture.files(R"(#pragma once
#include <cstddef>
#include <new>
int pick(long);
struct Box { Box(); int n; };
int pick(int);
void *operator new(std::size_t, long);
int allocate();
void *operator new(std::size_t, int);
int choose(long);
int lambda_call(int);
int choose(int);
int safe();
)", R"(Box::Box() : n(pick(0)) {}
int allocate() { int *p = new(1) int(7); return *p; }
int lambda_call(int input) { auto call = [input]() { return choose(input); }; return call(); }
int safe() { return 7; }
)");
  const auto data = fixture.generate();
  CHECK(fixture.reason(data, "Box::Box").find("changed_semantics:") == 0);
  CHECK(fixture.reason(data, "allocate").find("changed_semantics:") == 0);
  CHECK(fixture.reason(data, "lambda_call").find("changed_semantics:") == 0);
  CHECK(fixture.reason(data, "safe") == "accepted");
  CHECK(data.at("work").at("typeCacheHits").get<unsigned>() > 0);
  CHECK(data.at("work").at("finalParses") == 1);
}

TEST_CASE("constructors operators captures new and delete stitch with runtime checks",
          "[stitching][expanded][runtime]") {
  StitchFixture fixture("expanded_runtime");
  fixture.files(R"(#pragma once
struct Base { int base; int *count; Base(int, int *); ~Base(); };
struct Box : Base {
  int value; int *count;
  Box(int, int *); ~Box();
  Box &operator+=(int); explicit operator int() const;
};
int exercise();
)", R"(Base::Base(int input, int *out) : base(input + 1), count(out) {}
Base::~Base() { *count = *count * 10 + 1; }
Box::Box(int input, int *out) : Base(input, out), value(input * 2), count(out) {}
Box::~Box() { *count = *count * 10 + 2; }
Box &Box::operator+=(int input) { value += input; return *this; }
Box::operator int() const { return base + value; }
int exercise() {
  int destroyed = 0;
  int result = 0;
  { Box box(3, &destroyed); box += 2;
    auto value = [&, bonus = 1]() { return static_cast<int>(box) + bonus; };
    int *copy = new int(value()); result = *copy; delete copy;
  }
  return result + destroyed;
}
)");
  const auto data = fixture.generate();
  for (const auto &candidate : data.at("candidates"))
    CHECK(candidate.at("reason") == "accepted");
  CHECK(data.at("work").at("trialParses") == 1);
  CHECK(data.at("work").at("finalParses") == 1);
  fixture.runtime("int one() { return exercise(); }\n",
                  "int two() { return exercise(); }\n",
                  "int one(); int two(); int main() { return one() == 34 && two() == 34 ? 0 : 1; }\n");
}

TEST_CASE("preserved body branches object macros and inherited nodiscard are supported",
          "[stitching][expanded][preprocessor]") {
  StitchFixture fixture("expanded_preprocessor");
  fixture.files("#pragma once\n#define VALUE 7\n[[nodiscard]] int value();\n",
                "int value() {\n#if MODE\nreturn VALUE;\n#else\nreturn VALUE + 1;\n#endif\n}\n");
  const auto data = fixture.generate("all", "--frontend-arg -DMODE=1");
  CHECK(fixture.reason(data, "value") == "accepted");
  const auto text = read_text(fixture.output);
  CHECK(text.find("#if MODE") != std::string::npos);
  CHECK(text.find("return VALUE + 1;") != std::string::npos);
}

TEST_CASE("rewritten comparisons reject a different selected equality function",
          "[stitching][expanded][lookup]") {
  StitchFixture fixture("rewritten_comparison");
  fixture.files("#pragma once\nstruct L {}; struct R {};\n"
                "bool operator==(L, R);\nint run();\nbool operator==(R, L);\n",
                "int run() { return R{} != L{}; }\n");
  const auto data = fixture.generate("all", "--frontend-arg -std=c++20");
  CHECK(fixture.reason(data, "run").find("changed_semantics:") == 0);
}

TEST_CASE("matching feature branches retain their guards while mismatches stay put",
          "[stitching][expanded][preprocessor]") {
  StitchFixture fixture("matching_guards");
  fixture.files("#ifndef API_HPP\n#define API_HPP\n#if FEATURE\nint yes();\n#else\nint no();\n#endif\n"
                "#if FEATURE\nint mismatch();\n#endif\n#endif\n",
                "#if FEATURE\nint yes() { return 1; }\n#else\nint no() { return 2; }\n#endif\n"
                "#if FEATURE == 1\nint mismatch() { return 3; }\n#endif\n");
  auto data = fixture.generate("all", "--frontend-arg -DFEATURE=1");
  CHECK(fixture.reason(data, "yes") == "accepted");
  CHECK(fixture.reason(data, "mismatch") == "conditional_region");
  CHECK(read_text(fixture.output).find("#if FEATURE") != std::string::npos);
  data = fixture.generate("all", "--frontend-arg -DFEATURE=0");
  CHECK(fixture.reason(data, "no") == "accepted");
  CHECK(read_text(fixture.output).find("#else") != std::string::npos);
}

TEST_CASE("unsupported alternate-context bodies and internal templates remain deferred",
          "[stitching][expanded][contexts]") {
  StitchFixture fixture("alternate_unsupported");
  fixture.files("#pragma once\nint run();\nint safe();\nint hidden();\n",
                "int run() {\n#if GENERIC\nauto call = [](auto x) { return x; }; return call(1);\n"
                "#else\nreturn 1;\n#endif\n}\n"
                "namespace { template<class T> int helper(T x) { return x; } }\n"
                "int hidden() { return helper(2); }\n");
  write_text(fixture.include / "safe.cpp", "#include \"api.hpp\"\nint safe() { return 7; }\n");
  const auto database = fixture.work / "compile_commands.json";
  nlohmann::json commands = nlohmann::json::array();
  for (const auto &file : {"api.cpp", "safe.cpp"})
    commands.push_back({{"directory", fixture.work.generic_string()},
                        {"file", std::string("include/") + file},
                        {"arguments", {"clang++", "-std=c++17",
                                       file == std::string("api.cpp") ? "-DGENERIC=0" : "-DGENERIC=1",
                                       "-c", std::string("include/") + file}}});
  write_text(database, commands.dump());
  auto command = fixture.command("all", "--compilation-database " + quote(database));
  const std::string explicit_context = " --frontend-arg -std=c++17";
  command.erase(command.find(explicit_context), explicit_context.size());
  run_command(command);
  const auto data = nlohmann::json::parse(read_text(fixture.report));
  CHECK(data.at("baseline") == "valid");
  CHECK(fixture.reason(data, "run") != "accepted");
  CHECK(fixture.reason(data, "hidden") == "internal_dependency");
  CHECK(fixture.reason(data, "safe") == "accepted");
  CHECK(data.at("work").at("finalParses") == 2);
  CHECK(data.at("counts").at("removedBodies") == 1);
}

TEST_CASE("guided partition reuses an identical failed edit set after reordering",
          "[stitching][expanded][performance]") {
  StitchFixture fixture("guided_partition");
  std::string header = "#pragma once\nint bad();\n";
  std::string source = "int helper() { return 1; }\nint bad() { return helper(); }\n";
  for (int i = 0; i < 8; ++i) {
    const auto name = "good" + std::to_string(i);
    header += "int " + name + "();\n";
    source += "int " + name + "() { return " + std::to_string(i) + "; }\n";
  }
  fixture.files(header, source);
  const auto data = fixture.generate();
  CHECK(fixture.reason(data, "bad") == "destination_parse_error");
  CHECK(data.at("counts").at("removedBodies") == 8);
  CHECK(data.at("work").at("guidedPartitions") == 1);
  CHECK(data.at("work").at("trialParses") == 2);
  CHECK(data.at("work").at("trialCacheHits") == 1);
  CHECK(data.at("work").at("finalParses") == 1);
}

TEST_CASE("parallel contexts reproduce serial output, decisions and diagnostics",
          "[stitching][contexts][jobs]") {
  StitchFixture fixture("parallel_contexts");
  std::string header = "#pragma once\nint bad();\nint run();\n";
  std::string source = "int helper() { return 1; }\nint bad() { return helper(); }\n"
                       "int run() {\n#if GENERIC\nauto call = [](auto x) { return x; }; return call(1);\n"
                       "#else\nreturn 1;\n#endif\n}\n";
  nlohmann::json commands = nlohmann::json::array();
  auto add_context = [&](const std::string &file, const char *generic) {
    commands.push_back({{"directory", fixture.work.generic_string()},
                        {"file", "include/" + file},
                        {"arguments", {"clang++", "-std=c++17", std::string("-DGENERIC=") + generic,
                                       "-c", "include/" + file}}});
  };
  add_context("api.cpp", "0");
  for (int i = 0; i < 5; ++i) {
    const auto name = "good" + std::to_string(i);
    header += "int " + name + "();\n";
    write_text(fixture.include / (name + ".cpp"), "#include \"api.hpp\"\nint " + name +
               "() { return " + std::to_string(i) + "; }\n");
    add_context(name + ".cpp", i == 3 ? "1" : "0");
  }
  fixture.files(header, source);
  const auto database = fixture.work / "compile_commands.json";
  write_text(database, commands.dump());
  auto command = fixture.command("all", "--compilation-database " + quote(database));
  const std::string explicit_context = " --frontend-arg -std=c++17";
  command.erase(command.find(explicit_context), explicit_context.size());

  run_command(command + " --jobs 1");
  const auto serial_output = read_text(fixture.output);
  auto serial = nlohmann::json::parse(read_text(fixture.report));
  run_command(command + " --jobs 4");
  const auto parallel_output = read_text(fixture.output);
  auto parallel = nlohmann::json::parse(read_text(fixture.report));

  CHECK(serial.at("work").at("jobs") == 1);
  CHECK(parallel.at("work").at("jobs") == 4);
  CHECK(fixture.reason(serial, "bad") == "destination_parse_error");
  CHECK(fixture.reason(serial, "run") != "accepted");
  CHECK(fixture.reason(serial, "good4") == "accepted");
  CHECK(parallel_output == serial_output);
  for (auto *report : {&serial, &parallel}) {
    for (const auto *volatile_field : {"phaseMilliseconds", "analysisMilliseconds", "work"})
      report->erase(volatile_field);
  }
  CHECK(parallel == serial);
}

TEST_CASE("defaulted constructors and global allocation replacements stay deferred",
          "[stitching][expanded]") {
  StitchFixture fixture("special_function_boundaries");
  fixture.files("#pragma once\n#include <cstddef>\nstruct Box { Box(); };\n"
                "void *operator new(std::size_t);\nint safe();\n",
                "Box::Box() = default;\nvoid *operator new(std::size_t) { throw 1; }\n"
                "int safe() { return 7; }\n");
  const auto data = fixture.generate();
  CHECK(fixture.reason(data, "Box::Box") == "defaulted_or_deleted_function");
  CHECK(fixture.reason(data, "operator new") == "global_allocation_replacement");
  CHECK(fixture.reason(data, "safe") == "accepted");
}

TEST_CASE("include hoisting participates in stitched exact output and report bytes", "[stitching][includes]") {
  StitchFixture fixture("stitched_includes");
  fixture.files("#ifndef STITCH_GUARD\n#define STITCH_GUARD\nstruct Marker {};\n"
      "#include <cstddef>\nint safe();\n#endif\n",
      "#include <cstddef>\nint safe() { return sizeof(std::size_t); }\n");
  const std::string flags = "--deduplicate-includes --include-guard STITCH_GUARD";
  const auto data = fixture.generate("free", flags);
  CHECK(data.at("baseline") == "valid");
  CHECK(fixture.reason(data, "safe") == "accepted");
  const auto stitched = read_text(fixture.output);
  CHECK(stitched == "#pragma once\n#ifndef STITCH_GUARD\n#define STITCH_GUARD\n#include <cstddef>\n"
      "struct Marker {};\ninline int safe() { return sizeof(std::size_t); }\n#endif\n\n");
  CHECK(stitched.find("#define STITCH_GUARD\n#include <cstddef>\nstruct Marker {};\n") != std::string::npos);
  const auto first = stitched.find("#include <cstddef>");
  REQUIRE(first != std::string::npos);
  CHECK(stitched.find("#include <cstddef>", first + 1) == std::string::npos);
  CHECK(stitched.find("inline int safe() { return sizeof(std::size_t); }") != std::string::npos);
  run_command(fixture.command("off", flags));
  const auto unstitched = read_text(fixture.output);
  CHECK(data.at("bytes").at("unstitched") == unstitched.size());
  CHECK(data.at("bytes").at("stitched") == stitched.size());
  CHECK(data.at("bytes").at("savings") == static_cast<std::int64_t>(unstitched.size()) -
      static_cast<std::int64_t>(stitched.size()));
  run_command(fixture.command("free", flags));
  CHECK(read_text(fixture.output) == stitched);
}

TEST_CASE("an invalid include-hoisted baseline explicitly prevents relocation", "[stitching][includes][errors]") {
  StitchFixture fixture("invalid_hoisted_baseline");
  const auto vendor = fixture.work / "vendor";
  write_text(vendor / "dependent.hpp", "#ifndef REQUIRED_MODE\n#error include requires its macro\n#endif\n");
  fixture.files("#ifndef VALIDATE_GUARD\n#define VALIDATE_GUARD\n#define REQUIRED_MODE 1\n"
      "#include <dependent.hpp>\nint safe();\n#endif\n", "int safe() { return 1; }\n");
  const auto flags = "--deduplicate-includes --include-guard VALIDATE_GUARD --frontend-arg " +
      quote_value("-I" + vendor.generic_string());
  const auto data = fixture.generate("free", flags);
  CHECK(data.at("baseline") == "invalid");
  CHECK(data.at("baselineDiagnostics").get<std::string>().find("include requires its macro") != std::string::npos);
  CHECK(fixture.reason(data, "safe") == "baseline_invalid");
  CHECK(data.at("counts").at("removedBodies") == 0);
  const auto unchanged = read_text(fixture.output);
  CHECK(data.at("bytes").at("stitched") == unchanged.size());
  CHECK(data.at("bytes").at("unstitched") == unchanged.size());
  run_command(fixture.command("off", flags));
  CHECK(read_text(fixture.output) == unchanged);
}

TEST_CASE("mixed-include audit fails before stitching discovery without clobbering artifacts", "[stitching][includes][audit]") {
  StitchFixture fixture("mixed_include_discovery");
  fixture.files("#ifndef DISCOVERY_GUARD\n#define DISCOVERY_GUARD\n#include <thread>\n"
      "#if FEATURE\n#include <thread>\n#endif\nint safe();\n#endif\n", "this is deliberately invalid C++;\n");
  write_text(fixture.output, "output sentinel");
  write_text(fixture.report, "report sentinel");
  const auto result = run_result(fixture.command("free", "--deduplicate-includes --include-guard DISCOVERY_GUARD"));
  CHECK(result.status != 0);
  CHECK(result.error.find("<thread>: unguarded") != std::string::npos);
  CHECK(result.error.find("stitching discovering") == std::string::npos);
  CHECK(result.error.find("original analysis failed") == std::string::npos);
  CHECK(read_text(fixture.output) == "output sentinel");
  CHECK(read_text(fixture.report) == "report sentinel");
}


TEST_CASE("recursive namespace cleanup participates in stitched final output and exact report bytes", "[stitching][namespaces][runtime]") {
  StitchFixture fixture("stitched_namespaces");
  fixture.files("#ifndef STITCH_NAMESPACE_GUARD\n#define STITCH_NAMESPACE_GUARD\n"
      "namespace api { namespace middle { namespace leaf { struct Marker {}; } } }\n"
      "#include <cstddef>\n"
      "namespace api { namespace middle { namespace leaf { int safe(); } } }\n/** INSERT_CSV_SOURCES **/\n#endif\n",
      "namespace api { namespace middle { namespace leaf { int safe() { return sizeof(std::size_t); } } } }\n");
  const std::string flags = "--merge-namespaces --deduplicate-includes --include-guard STITCH_NAMESPACE_GUARD";
  const auto data = fixture.generate("free", flags);
  CHECK(data.at("baseline") == "valid");
  CHECK(fixture.reason(data, "api::middle::leaf::safe") == "accepted");
  const auto stitched = read_text(fixture.output);
  for (const auto *name : {"api", "middle", "leaf"}) {
    const auto word = std::string("namespace ") + name;
    const auto first = stitched.find(word);
    REQUIRE(first != std::string::npos);
    CHECK(stitched.find(word, first + word.size()) == std::string::npos);
  }
  run_command(fixture.command("off", flags));
  const auto unstitched = read_text(fixture.output);
  CHECK(data.at("bytes").at("unstitched") == unstitched.size());
  CHECK(data.at("bytes").at("stitched") == stitched.size());
  CHECK(data.at("bytes").at("savings") == static_cast<std::int64_t>(unstitched.size()) - static_cast<std::int64_t>(stitched.size()));
  run_command(fixture.command("free", flags));
  CHECK(read_text(fixture.output) == stitched);
  fixture.runtime("int one() { return api::middle::leaf::safe(); }\n",
      "int two() { return api::middle::leaf::safe(); }\n",
      "int one(); int two(); int main() { return one() > 0 && one() == two() ? 0 : 1; }\n");
}


TEST_CASE("source location values reject changed residual neighbors without a global veto", "[stitching][source-location]") {
  StitchFixture fixture("source_location_body");
  fixture.files("#pragma once\nint safe(); inline int column() { return __builtin_COLUMN(); }\nint untouched();\n",
      "int safe() { return 7; }\nint untouched() { return 9; }\n");
  const auto data = fixture.generate("free");
  CHECK(data.at("baseline") == "valid");
  CHECK(fixture.reason(data, "safe").find("changed_semantics:") == 0);
  CHECK(fixture.reason(data, "untouched") == "accepted");
  CHECK(data.at("counts").at("acceptedFree") == 1);
  CHECK(read_text(fixture.output).find("int safe(); inline int column() { return __builtin_COLUMN(); }") != std::string::npos);
}

TEST_CASE("source location values participate in default variable and field declaration records", "[stitching][source-location]") {
  const std::vector<std::pair<std::string, std::string>> declarations = {
      {"default", "int observed(int input = __builtin_COLUMN());"},
      {"variable", "inline unsigned observed = __builtin_COLUMN();"},
      {"field", "struct Observed { unsigned value = __builtin_COLUMN(); };"}};
  for (const auto &[name, declaration] : declarations) {
    INFO(name);
    StitchFixture fixture("source_location_" + name);
    fixture.files("#pragma once\nint safe(); " + declaration + "\nint untouched();\n",
        "int safe() { return 7; }\nint untouched() { return 9; }\n");
    const auto data = fixture.generate("free");
    CHECK(data.at("baseline") == "valid");
    CHECK(fixture.reason(data, "safe").find("changed_declaration:" + name + ":") == 0);
    CHECK(fixture.reason(data, "untouched") == "accepted");
  }
}

TEST_CASE("source location defaults retain their actual call and aggregate initialization contexts", "[stitching][source-location]") {
  const std::vector<std::pair<std::string, std::string>> contexts = {
    {"argument", "inline unsigned observed(unsigned value = __builtin_COLUMN()) { return value; }\n"
                 "int safe(); inline unsigned column() { return observed(); }\n"},
    {"initializer", "struct Observed { unsigned value = __builtin_COLUMN(); };\n"
                    "int safe(); inline unsigned column() { Observed object{}; return object.value; }\n"},
    {"array", "struct Observed { unsigned value = __builtin_COLUMN(); };\n"
              "int safe(); inline unsigned column() { Observed objects[2]{}; return objects[1].value; }\n"},
    {"nested", "namespace std { struct source_location { struct __impl { const char *_M_file_name; "
               "const char *_M_function_name; unsigned _M_line; unsigned _M_column; }; "
               "const __impl *data; static constexpr source_location current(const __impl *p = __builtin_source_location()) { return {p}; } "
               "constexpr unsigned column() const { return data->_M_column; } }; }\n"
               "inline unsigned observed(std::source_location location = std::source_location::current()) { return location.column(); }\n"
               "int safe(); inline unsigned column() { return observed(); }\n"}};
  for (const auto &[name, header] : contexts) {
    INFO(name);
    StitchFixture fixture("source_location_context_" + name);
    fixture.files("#pragma once\n" + header + "int untouched();\n",
        "int safe() { return 7; }\nint untouched() { return 9; }\n");
    const auto data = fixture.generate("free");
    CHECK(data.at("baseline") == "valid");
    CHECK(fixture.reason(data, "safe").find("changed_semantics:") == 0);
    CHECK(fixture.reason(data, "untouched") == "accepted");
  }
}


TEST_CASE("contained function macros preserve nested invocations and control flow targets", "[stitching][body-macros][runtime]") {
  StitchFixture fixture("contained_function_macros");
  fixture.files("#pragma once\n#define ADD(a, b) ((a) + (b))\n#define PASS(...) __VA_ARGS__\n"
      "#define STOP_IF(condition) if (condition) break\n#define RETURN_VALUE(value) return (value)\n"
      "int exercise(int);\nstruct Counter { int run(int); };\n",
      "int exercise(int limit) { int sum = 0; for (int i = 0; i < limit; ++i) { "
      "PASS(STOP_IF(i == 3)); sum = ADD(sum, PASS(ADD(i, 1))); } PASS(return sum;); }\n"
      "int Counter::run(int limit) { int sum = 0; for (int i = 0; i < limit; ++i) { "
      "for (int j = 0; j < 4; ++j) { STOP_IF(j == 2); sum += ADD(i, j); } } RETURN_VALUE(sum); }\n");
  const auto data = fixture.generate();
  CHECK(fixture.reason(data, "exercise") == "accepted");
  CHECK(fixture.reason(data, "Counter::run") == "accepted");
  const auto output = read_text(fixture.output);
  CHECK(output.find("sum = ADD(sum, PASS(ADD(i, 1)))") != std::string::npos);
  CHECK(output.find("STOP_IF(j == 2)") != std::string::npos);
  CHECK(output.find("RETURN_VALUE(sum)") != std::string::npos);
  fixture.runtime("int one() { return exercise(8); }\n",
      "int two() { return Counter{}.run(3); }\n",
      "int one(); int two(); int main() { return one() == 6 && two() == 9 ? 0 : 1; }\n");
}

TEST_CASE("variadic catch macros preserve named bindings and erased handler spelling in every context", "[stitching][body-macros][contexts][runtime]") {
  StitchFixture fixture("variadic_catch_macros");
  const std::string header = R"(#pragma once
#ifndef CSV_EXCEPTIONS_ENABLED
#define CSV_EXCEPTIONS_ENABLED 1
#endif
#if CSV_EXCEPTIONS_ENABLED
#define CSV_TRY try
#define CSV_CATCH(exception_declaration, ...) catch exception_declaration __VA_ARGS__
template<class A, class B> struct Failure { int value; };
#else
#define CSV_TRY
#define CSV_CATCH(exception_declaration, ...)
#endif
#define WRAP(...) __VA_ARGS__
#define PLUS(a, b) ((a) + (b))
int guarded(int);
int rethrowing();
)";
  const std::string body = R"({
  int answer = 0;
  CSV_TRY {
#if CSV_EXCEPTIONS_ENABLED
    WRAP(if (input < 0) throw Failure<int, long>{PLUS(4, WRAP(5))};)
#endif
    answer = PLUS(input, WRAP(2));
  } CSV_CATCH((const Failure<int, long>& failure), { return PLUS(failure.value, WRAP(3)); })
  return answer;
})";
  fixture.files(header, "int guarded(int input) " + body + R"(
int rethrowing() {
#if CSV_EXCEPTIONS_ENABLED
  try {
    CSV_TRY { WRAP(throw Failure<int, long>{9};) }
    CSV_CATCH((const Failure<int, long>& failure), { (void)failure; WRAP(throw;); })
  } catch (const Failure<int, long>& outer) { return outer.value; }
#else
  return 9;
#endif
}
)");
  // One physical definition is discovered under both exception configurations.
  write_text(fixture.include / "other.cpp", "#include \"api.hpp\"\n");
  const auto database = fixture.work / "compile_commands.json";
  nlohmann::json commands = nlohmann::json::array();
  for (const auto &file : {"api.cpp", "other.cpp"})
    commands.push_back({{"directory", fixture.work.generic_string()}, {"file", (fixture.include / file).generic_string()},
      {"arguments", {"clang++", "-std=c++17", std::string("-DCSV_EXCEPTIONS_ENABLED=") + (std::string(file) == "api.cpp" ? "1" : "0"),
                     (fixture.include / file).generic_string()}}});
  write_text(database, commands.dump());
  const auto command = quote(tool_path()) + " --include-root " + quote(fixture.include) + " --output " + quote(fixture.output) +
      " --function-stitching all --compilation-database " + quote(database) + " --stitching-report " + quote(fixture.report);
  run_command(command);
  const auto data = nlohmann::json::parse(read_text(fixture.report));
  CHECK(data.at("baseline") == "valid");
  CHECK(fixture.reason(data, "guarded") == "accepted");
  CHECK(fixture.reason(data, "rethrowing") == "accepted");
  CHECK(data.at("work").at("finalParses") == 2);
  const auto output = read_text(fixture.output);
  CHECK(output.find(body) != std::string::npos);
  CHECK(data.at("bytes").at("stitched") == output.size());
  run_command(quote(tool_path()) + " --include-root " + quote(fixture.include) + " --output " + quote(fixture.output));
  const auto unstitched = read_text(fixture.output);
  CHECK(data.at("bytes").at("unstitched") == unstitched.size());
  CHECK(data.at("bytes").at("savings") == static_cast<std::int64_t>(unstitched.size()) - static_cast<std::int64_t>(output.size()));
  run_command(command);
  CHECK(read_text(fixture.output) == output);
  fixture.runtime("int one() { return guarded(-1) + rethrowing(); }\n",
      "int two() { return guarded(5); }\n",
      "int one(); int two(); int main() { return one() == 21 && two() == 7 ? 0 : 1; }\n",
      "#define CSV_EXCEPTIONS_ENABLED 1\n");

  // The exception-only type and named variable are deliberately unavailable;
  // the erased argument still must be copied whole and compile without EH.
  // Compiler-disabled EH is verified by discovery/trial/final in this context.
  run_command(fixture.command("all", "--frontend-arg -DCSV_EXCEPTIONS_ENABLED=0 --frontend-arg -fno-exceptions"));
  const auto disabled_data = nlohmann::json::parse(read_text(fixture.report));
  CHECK(fixture.reason(disabled_data, "guarded") == "accepted");
  CHECK(fixture.reason(disabled_data, "rethrowing") == "accepted");
  CHECK(read_text(fixture.output).find(body) != std::string::npos);
  fixture.runtime("int one() { return guarded(-1) + rethrowing(); }\n",
      "int two() { return guarded(5); }\n",
      "int one(); int two(); int main() { return one() == 10 && two() == 7 ? 0 : 1; }\n", {},
      "target_compile_definitions(consumer PRIVATE CSV_EXCEPTIONS_ENABLED=0)\n"
      "if(MSVC)\ntarget_compile_options(consumer PRIVATE /EHs-c-)\n"
      "else()\ntarget_compile_options(consumer PRIVATE -fno-exceptions)\nendif()\n");
}

TEST_CASE("function macro containment rejects generated function boundaries and escaping invocations", "[stitching][body-macros][fallback]") {
  const std::vector<std::pair<std::string, std::string>> definitions = {
    {"closing", "#define CLOSE() }\nint value() { return 7; CLOSE()\n"},
    {"opening", "#define OPEN() {\nint value() OPEN() return 7; }\n"},
    {"whole", "#define DEFINE(name, ...) int name() { __VA_ARGS__ }\nDEFINE(value, return 7;)\n"},
    {"argument_close", "#define FORWARD(...) __VA_ARGS__\nint value() { FORWARD(return 7; })\n"},
    {"escape", "#define ESCAPE(...) } __VA_ARGS__ int other() {\nint value() { return 7; ESCAPE(int emitted;) return 9; }\n"}};
  for (const auto &[name, source] : definitions) {
    INFO(name);
    StitchFixture fixture("macro_boundary_" + name);
    fixture.files("#pragma once\nint value();\n", source);
    const auto data = fixture.generate();
    CHECK(fixture.reason(data, "value") != "accepted");
    CHECK(data.at("counts").at("removedBodies") == 0);
  }
}

TEST_CASE("function macros in signatures initializers and pragma-producing bodies stay deferred", "[stitching][body-macros][fallback]") {
  StitchFixture fixture("macro_effects");
  fixture.files("#pragma once\n#define PASS(...) __VA_ARGS__\n#define EFFECT() _Pragma(\"pack(push, 1)\")\n"
      "#define RESTORE() _Pragma(\"pack(pop)\")\n"
      "#define ALIAS() EFFECT()\nint value();\nint direct();\nstruct Box { Box(); int value; };\nint signed_value();\n",
      "int value() { ALIAS(); RESTORE(); return 7; }\n"
      "int direct() { _Pragma(\"pack(push, 1)\") _Pragma(\"pack(pop)\") return 8; }\n"
      "Box::Box() : value(PASS(3)) {}\nint signed_value PASS(()) { return 9; }\n");
  const auto data = fixture.generate();
  CHECK(fixture.reason(data, "value") == "body_macro");
  CHECK(fixture.reason(data, "direct") == "body_macro");
  CHECK(fixture.reason(data, "Box::Box") != "accepted");
  CHECK(fixture.reason(data, "signed_value") != "accepted");
  CHECK(data.at("counts").at("removedBodies") == 0);
}

TEST_CASE("function macros still reject destination redefinitions and changed lookup", "[stitching][body-macros][lookup]") {
  StitchFixture fixture("macro_changed_lookup");
  fixture.files("#pragma once\n#define APPLY(value) pick(value)\nint pick(long);\nint run();\nint pick(int);\n"
      "#define VALUE(value) ((value) + 1)\nint changed();\nint ordinary();\n",
      "#undef VALUE\n#define VALUE(value) ((value) + 2)\n"
      "int run() { return APPLY(0); }\nint changed() { return VALUE(4); }\nint ordinary() { return 7; }\n");
  const auto data = fixture.generate();
  CHECK(fixture.reason(data, "run").find("changed_semantics:") == 0);
  CHECK(fixture.reason(data, "changed").find("changed_semantics:") == 0);
  CHECK(fixture.reason(data, "ordinary") == "accepted");
}
