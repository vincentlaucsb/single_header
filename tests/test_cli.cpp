#include "test_support.hpp"

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
