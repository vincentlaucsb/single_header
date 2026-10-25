#include "test_support.hpp"

TEST_CASE("version uses the project version without loading configuration", "[version]") {
  const auto result = run_result(quote(tool_path()) + " --version --config " +
      quote(output_dir() / "nonexistent-config.json"));
  CHECK(result.status == 0);
  const std::string line = "single_header " SINGLE_HEADER_TEST_VERSION;
  CHECK((result.out == line + "\n" || result.out == line + "\r\n"));
  CHECK(result.error.empty());
}

TEST_CASE("generates golden output for a small fake library", "[golden]") {
  expect_golden("simple");
}

TEST_CASE("generates golden output for csv-parser style fixture", "[golden]") {
  expect_golden("csv_style",
                "--rewrite-macro CSV_INLINE=inline --implementation-marker " +
                  quote_value("/** INSERT_CSV_SOURCES **/"));
}

TEST_CASE("known project headers deduplicate diamond quote and angle includes", "[cleanup][includes]") {
  const auto work = output_dir() / "include_diamond";
  const auto include = work / "include";
  fs::create_directories(include);
  write_text(include / "shared.hpp", "#pragma once\nstruct SharedDiamond { int value; };\n");
  write_text(include / "left.hpp", "#pragma once\n#include \"shared.hpp\"\n");
  write_text(include / "right.hpp", "#pragma once\n#include <shared.hpp>\n");
  write_text(include / "api.hpp", "#pragma once\n#include \"left.hpp\"\n#include \"right.hpp\"\n"
      "#include \"shared.hpp\"\n#include <shared.hpp>\n#include <vendor.hpp>\n#include <vendor.hpp>\n");
  const auto output = work / "generated.hpp";
  run_command(quote(tool_path()) + " --include-root " + quote(include) +
              " --entry " + quote(include / "api.hpp") + " --output " + quote(output));
  const auto text = read_text(output);
  const auto marker = text.find("struct SharedDiamond");
  REQUIRE(marker != std::string::npos);
  CHECK(text.find("struct SharedDiamond", marker + 1) == std::string::npos);
  CHECK(text.find("#include \"shared.hpp\"") == std::string::npos);
  CHECK(text.find("#include <shared.hpp>") == std::string::npos);
  CHECK(text.find("#include <vendor.hpp>\n#include <vendor.hpp>") != std::string::npos);
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

TEST_CASE("explicit off overrides configured stitching and preserves legacy output", "[stitching][config]") {
  const auto work = output_dir() / "off_override";
  const auto fixture = fixture_dir() / "simple";
  const auto config = work / "config.json";
  write_text(config, nlohmann::json{{"includeRoots", { (fixture / "include").generic_string() }},
      {"output", (work / "result.hpp").generic_string()}, {"functionStitching", "all"}}.dump());
  run_command(quote(tool_path()) + " --config " + quote(config) + " --function-stitching off");
  CHECK(read_text(work / "result.hpp") == read_text(fixture / "expected.hpp"));
}

TEST_CASE("stitching rejects invalid modes and unavailable compilation contexts", "[stitching][errors]") {
  const auto args = quote(tool_path()) + " --include-root " + quote(fixture_dir() / "simple/include") +
      " --output " + quote(output_dir() / "errors.hpp");
  auto invalid = run_result(args + " --function-stitching automatic");
  CHECK(invalid.status != 0); CHECK(invalid.error.find("expects off, free, methods, or all") != std::string::npos);
  auto unavailable = run_result(args + " --function-stitching all");
  CHECK(unavailable.status != 0);
#ifdef SINGLE_HEADER_TEST_WITH_CLANG
  CHECK(unavailable.error.find("requires exactly one") != std::string::npos);
#else
  CHECK(unavailable.error.find("SINGLE_HEADER_WITH_CLANG=ON") != std::string::npos);
#endif
}

TEST_CASE("include hoisting CLI and JSON options override independently", "[config][includes]") {
  const auto work = output_dir() / "include_options";
  const auto include = work / "include";
  const auto output = work / "result.hpp";
  const auto config = work / "config.json";
  write_text(include / "api.hpp", "#ifndef CONFIG_GUARD\n#define CONFIG_GUARD\n"
      "int value;\n#include <vector>\n#include <vector>\n#endif\n");
  auto settings = nlohmann::json{{"includeRoots", {include.generic_string()}},
      {"output", output.generic_string()}, {"deduplicateIncludes", true}, {"includeGuard", "CONFIG_GUARD"}};
  write_text(config, settings.dump());
  const auto command = quote(tool_path()) + " --config " + quote(config);
  run_command(command);
  CHECK(read_text(output) == "#pragma once\n#ifndef CONFIG_GUARD\n#define CONFIG_GUARD\n"
      "#include <vector>\nint value;\n#endif\n");
  run_command(command + " --no-deduplicate-includes --include-guard IGNORED");
  CHECK(read_text(output).find("int value;\n#include <vector>\n#include <vector>\n") != std::string::npos);
  settings["deduplicateIncludes"] = false;
  settings["includeGuard"] = "WRONG_GUARD";
  write_text(config, settings.dump());
  run_command(command + " --deduplicate-includes --include-guard CONFIG_GUARD");
  CHECK(read_text(output).find("#define CONFIG_GUARD\n#include <vector>\nint value;") != std::string::npos);
  auto wrong_guard = run_result(command + " --deduplicate-includes");
  CHECK(wrong_guard.status != 0);
  CHECK(wrong_guard.error.find("configured outer guard WRONG_GUARD") != std::string::npos);
  settings["deduplicateIncludes"] = "true";
  write_text(config, settings.dump());
  auto invalid_boolean = run_result(command);
  CHECK(invalid_boolean.status != 0);
  CHECK(invalid_boolean.error.find("must be a boolean") != std::string::npos);
  settings["deduplicateIncludes"] = true;
  settings.erase("includeGuard");
  write_text(config, settings.dump());
  auto missing_guard = run_result(command);
  CHECK(missing_guard.status != 0);
  CHECK(missing_guard.error.find("--include-guard") != std::string::npos);
}

TEST_CASE("mixed-include waivers override JSON and audit failures preserve existing artifacts", "[config][includes][audit]") {
  const auto work = output_dir() / "mixed_include_options";
  const auto include = work / "include";
  const auto output = work / "result.hpp";
  const auto report = work / "report.json";
  const auto config = work / "config.json";
  write_text(include / "api.hpp", "#ifndef MIXED_CONFIG_GUARD\n#define MIXED_CONFIG_GUARD\n"
      "int value;\n#include <thread>\n#include <vector>\n#if FEATURE\n#include <thread>\n#include <vector>\n#endif\n#endif\n");
  auto settings = nlohmann::json{{"includeRoots", {include.generic_string()}},
      {"output", output.generic_string()}, {"stitchingReport", report.generic_string()},
      {"deduplicateIncludes", true}, {"includeGuard", "MIXED_CONFIG_GUARD"}};
  write_text(config, settings.dump());
  write_text(output, "output sentinel");
  write_text(report, "report sentinel");
  const auto command = quote(tool_path()) + " --config " + quote(config);
  const auto strict = run_result(command);
  CHECK(strict.status != 0);
  CHECK(strict.error.find("<thread>: unguarded") != std::string::npos);
  CHECK(strict.error.find("<vector>: unguarded") != std::string::npos);
  CHECK(read_text(output) == "output sentinel");
  CHECK(read_text(report) == "report sentinel");
  settings["allowMixedIncludes"] = {"<thread>", "<vector>"};
  write_text(config, settings.dump());
  run_command(command);
  const auto hoisted = read_text(output);
  CHECK(hoisted == "#pragma once\n#ifndef MIXED_CONFIG_GUARD\n#define MIXED_CONFIG_GUARD\n"
      "#include <thread>\n#include <vector>\nint value;\n#if FEATURE\n#endif\n#endif\n");
  const auto overridden = run_result(command + " --allow-mixed-include " + quote_value("<thread>"));
  CHECK(overridden.status != 0);
  CHECK(overridden.error.find("<vector>: unguarded") != std::string::npos);
  CHECK(overridden.error.find("<thread>: unguarded") == std::string::npos);
  CHECK(read_text(output) == hoisted);
  run_command(command + " --allow-mixed-include " + quote_value("<thread>") +
      " --allow-mixed-include " + quote_value("<vector>"));
  CHECK(read_text(output) == hoisted);
  run_command(command + " --no-deduplicate-includes --allow-mixed-include " + quote_value("<unrelated>"));
  CHECK(read_text(output).find("#if FEATURE\n#include <thread>\n#include <vector>") != std::string::npos);
  settings["allowMixedIncludes"] = {42};
  write_text(config, settings.dump());
  const auto invalid = run_result(command);
  CHECK(invalid.status != 0);
  CHECK(invalid.error.find("allowMixedIncludes") != std::string::npos);
  CHECK(invalid.error.find("array of strings") != std::string::npos);
}


TEST_CASE("namespace merging JSON and CLI overrides preserve macro-boundary runtime binding", "[cleanup][namespaces][runtime][config]") {
  const auto work = output_dir() / "namespace_options";
  const auto include = work / "include";
  const auto output = work / "generated.hpp";
  fs::remove_all(work);
  fs::create_directories(include);
  const std::string input = "#pragma once\nnamespace api { namespace detail { namespace leaf { inline int first() { return 3; } } } }\n"
      "namespace api { namespace detail { namespace leaf { inline int second() { return first() + 4; } } } }\n"
      "#define SPLIT int marker; } namespace other { int\n"
      "namespace original { SPLIT moved = 5; } namespace original { inline int answer() { return 7; } }\n"
      "#undef SPLIT\n"
      "namespace CSV { enum Kind { CSV_BOOL = 4 }; template<class T> struct Box { T value; };\n"
      "#if FEATURE\ninline int chosen() { return 5; }\n#else\ninline int chosen() { return 3; }\n#endif\n}\n"
      "namespace CSV {\n#pragma region functions\ninline int result() { Box<int> box{CSV_BOOL}; return chosen() + box.value; }\n#pragma endregion\n}\n";
  write_text(include / "api.hpp", input);
  const auto config = work / "settings.json";
  nlohmann::json settings = {{"includeRoots", {include.generic_string()}}, {"output", output.generic_string()}};
  const auto command = quote(tool_path()) + " --config " + quote(config);
  write_text(config, settings.dump());
  run_command(command);
  const auto unmerged = read_text(output);
  CHECK(unmerged.find("namespace api", unmerged.find("namespace api") + 1) != std::string::npos);
  settings["mergeNamespaces"] = true;
  write_text(config, settings.dump());
  run_command(command);
  const auto merged = read_text(output);
  CHECK(merged.find("namespace api", merged.find("namespace api") + 1) == std::string::npos);
  CHECK(merged.find("namespace original { SPLIT moved = 5; } namespace original {") != std::string::npos);
  CHECK(merged.find("namespace CSV", merged.find("namespace CSV") + 1) == std::string::npos);
  run_command(command + " --no-merge-namespaces");
  CHECK(read_text(output) == unmerged);
  settings["mergeNamespaces"] = false;
  write_text(config, settings.dump());
  run_command(command + " --merge-namespaces");
  CHECK(read_text(output) == merged);
  settings["mergeNamespaces"] = "true";
  write_text(config, settings.dump());
  const auto invalid = run_result(command);
  CHECK(invalid.status != 0);
  CHECK(invalid.error.find("mergeNamespaces") != std::string::npos);
  CHECK(invalid.error.find("must be a boolean") != std::string::npos);

  const auto project = work / "consumer";
  write_text(project / "main.cpp", "#include \"generated.hpp\"\n"
      "int main() { return api::detail::leaf::second() == 7 && other::moved == 5 && original::answer() == 7 && CSV::result() == (FEATURE ? 9 : 7) ? 0 : 1; }\n");
  write_text(project / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\nproject(namespace_consumer LANGUAGES CXX)\n"
      "add_executable(consumer main.cpp)\ntarget_compile_features(consumer PRIVATE cxx_std_17)\n"
      "target_include_directories(consumer PRIVATE \"" + work.generic_string() + "\")\n"
      "target_compile_definitions(consumer PRIVATE FEATURE=0)\n"
      "add_executable(consumer_feature main.cpp)\ntarget_compile_features(consumer_feature PRIVATE cxx_std_17)\n"
      "target_include_directories(consumer_feature PRIVATE \"" + work.generic_string() + "\")\n"
      "target_compile_definitions(consumer_feature PRIVATE FEATURE=1)\n"
      "enable_testing()\nadd_test(NAME runtime COMMAND consumer)\nadd_test(NAME feature_runtime COMMAND consumer_feature)\n");
  run_command(quote(cmake_command()) + " -S " + quote(project) + " -B " + quote(work / "build") +
      " -DCMAKE_BUILD_TYPE=Release -G " + quote_value(cmake_generator()));
  run_command(quote(cmake_command()) + " --build " + quote(work / "build") + " --config Release");
  run_command(quote(cmake_command()) + " -E chdir " + quote(work / "build") + " ctest -C Release --output-on-failure");
}
