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
               const std::string &preamble = {}) const {
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
        "main.cpp)\ntarget_compile_features(consumer PRIVATE cxx_std_17)\n"
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
  CHECK(fixture.reason(data, "conditional_body") == "conditional_region");
  CHECK(fixture.reason(data, "macro_body") == "body_macro");
  CHECK(fixture.reason(data, "Box::virtual_method") == "virtual_method");
  CHECK(fixture.reason(data, "generic") == "template");
  CHECK(data.at("counts").at("removedBodies") == 0);
  const auto stitched = read_text(fixture.output);
  run_command(fixture.command("off"));
  CHECK(read_text(fixture.output) == stitched);
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
  CHECK(fixture.reason(data, "residual").find("unsupported_ast:") == 0);
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
  CHECK(
      fixture.reason(data, "demo::Names::set_names").find("unsupported_ast:") ==
      0);
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
  CHECK(fixture.reason(data, "residual").find("unsupported_ast:") == 0);
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
