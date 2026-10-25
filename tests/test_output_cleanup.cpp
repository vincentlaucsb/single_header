#include "test_support.hpp"
#include "../src/output_cleanup.hpp"
#include <catch2/matchers/catch_matchers_string.hpp>

using single_header::cleanup_output;

TEST_CASE("cleanup collapses ordinary blank lines and protects lexical payloads", "[cleanup]") {
  CHECK(cleanup_output("int a;\n\n  \n\t\nint b;\n") == "int a;\n\nint b;\n");
  const std::string protected_text = R"fixture(const char *raw = u8R"tag(first


#include <vector>
@file literal.hpp
)tag";
const char *quoted = "escaped quote: \" and slash: \\";
const char *continued = "first\
second";
char apostrophe = '\'';
int number = 1'000 + 0xCA'FE;
/* ordinary comment


@file untouched.hpp
*/
// ordinary @file untouched.hpp
)fixture";
  CHECK(cleanup_output(protected_text) == protected_text);
  CHECK(cleanup_output(protected_text + "\n\n\nint after;\n") == protected_text + "\nint after;\n");
  const std::string misleading_raw = "auto x = R\"tag(first)\\\ntag\"\n\n\nlast)tag\";\n";
  CHECK(cleanup_output(misleading_raw) == misleading_raw);
  CHECK(cleanup_output("auto x = R\"tag(unterminated\n\n\n@file keep\n") ==
        "auto x = R\"tag(unterminated\n\n\n@file keep\n");
  CHECK(cleanup_output("/* unterminated\n\n\n@file keep\n") == "/* unterminated\n\n\n@file keep\n");
}

TEST_CASE("cleanup preserves spliced comments and continued preprocessing lines", "[cleanup]") {
  const std::string source = "#define NUMBER 1'000\n#define BLOCK first \\\n\n\nlast\n"
      "// comment continues \\\n\n\nint after;\n"
      "/\\\n** @file escaped.hpp *\\\n/ int token;\n";
  CHECK(cleanup_output(source) == source);
  CHECK(cleanup_output("#define NUMBER 1'000\nint a;\n\n\nint b;\n") ==
        "#define NUMBER 1'000\nint a;\n\nint b;\n");
}

TEST_CASE("cleanup removes only Doxygen file directives and preserves other comment text", "[cleanup]") {
  const std::string source = R"fixture(/** @file api.hpp
 * @brief API documentation.
 * Copyright 2026 Example.
 * See @file in ordinary prose.
 */
int/** @file x.hpp */value;
/*! \file "path with spaces/api.hpp" @brief Other documentation. */
/// @file api.hpp
//! \file api.hpp
/// @file Copyright (c) Example
/** @file api.hpp Copyright (c) Example */
// @file ordinary.hpp
/* \file ordinary.hpp */
const char *text = "@file string.hpp";
)fixture";
  const auto output = cleanup_output(source);
  CHECK(output.find("@file api.hpp") == std::string::npos);
  CHECK(output.find("\\file \"path with spaces/api.hpp\"") == std::string::npos);
  CHECK(output.find("int/**  */value;") != std::string::npos);
  CHECK(output.find("@brief API documentation.") != std::string::npos);
  CHECK(output.find("Copyright 2026 Example.") != std::string::npos);
  CHECK(output.find("See @file in ordinary prose.") != std::string::npos);
  CHECK(output.find("Copyright (c) Example") != std::string::npos);
  CHECK(output.find("// @file ordinary.hpp") != std::string::npos);
  CHECK(output.find("/* \\file ordinary.hpp */") != std::string::npos);
  CHECK(output.find("\"@file string.hpp\"") != std::string::npos);
  CHECK(cleanup_output("/// @file eof.hpp") == "/// ");
}

TEST_CASE("cleanup preserves Doxygen code verbatim and Markdown fenced blocks", "[cleanup]") {
  const std::string source = R"fixture(/** @code{.cpp}
 * @file keep_code.hpp
 * @endcode
 * \verbatim
 * \file keep_verbatim.hpp
 * \endverbatim
 * ````cpp
 * ```
 * @file keep_fence.hpp
 * ~~~
 * @file keep_other_fence.hpp
 * ````
 * @file remove.hpp
 */
/// @code
/// @file keep_line.hpp
/// @endcode
/// @file remove_line.hpp
)fixture";
  const auto output = cleanup_output(source);
  CHECK(output.find("@file keep_code.hpp") != std::string::npos);
  CHECK(output.find("\\file keep_verbatim.hpp") != std::string::npos);
  CHECK(output.find("@file keep_fence.hpp") != std::string::npos);
  CHECK(output.find("@file keep_other_fence.hpp") != std::string::npos);
  CHECK(output.find("@file keep_line.hpp") != std::string::npos);
  CHECK(output.find("@file remove.hpp") == std::string::npos);
  CHECK(output.find("@file remove_line.hpp") == std::string::npos);
  const std::string uncertain = "/// @co\\\nde\n/// @file retain.hpp\n/// @endcode\n/// @file retain_too.hpp\n\n/// @file remove.hpp\n";
  CHECK(cleanup_output(uncertain).find("@file retain.hpp") != std::string::npos);
  CHECK(cleanup_output(uncertain).find("@file retain_too.hpp") != std::string::npos);
  CHECK(cleanup_output(uncertain).find("@file remove.hpp") == std::string::npos);
}

TEST_CASE("include deduplication defaults off and retains adjacent and distant repeats", "[cleanup][includes]") {
  const std::string source = "#include <vector>\n#include <vector>\nint value;\n"
      "#define MODE 1\n#include <vector>\n#if FEATURE\n#include <vector>\n#endif\n";
  CHECK(cleanup_output(source) == source);
}

TEST_CASE("optional include pass hoists file-wide literals in first-seen order", "[cleanup][includes]") {
  const std::string source = R"fixture(#ifndef LIBRARY_GUARD
#define LIBRARY_GUARD
#include <algorithm>
int first;
#define MODE 1
#include <vector>
#if FEATURE
#include <string>
#include <conditional_only.hpp>
#elif OTHER
#include <vector>
#else
#ifndef VENDOR_GUARD
#define VENDOR_GUARD
#include <vendor_only.hpp>
#include <algorithm>
#endif
#endif
struct Scope {
#include <string>
};
#undef MODE
#include <algorithm>
#endif
#include <vector>
)fixture";
  const auto output = cleanup_output(source, true, "LIBRARY_GUARD", {"<algorithm>", "<vector>", "<string>"});
  CHECK(output == R"fixture(#ifndef LIBRARY_GUARD
#define LIBRARY_GUARD
#include <algorithm>
#include <vector>
#include <string>
int first;
#define MODE 1
#if FEATURE
#include <conditional_only.hpp>
#elif OTHER
#else
#ifndef VENDOR_GUARD
#define VENDOR_GUARD
#include <vendor_only.hpp>
#endif
#endif
struct Scope {
};
#undef MODE
#endif
)fixture");
  CHECK(cleanup_output(output, true, "LIBRARY_GUARD") == output);
}

TEST_CASE("include identities retain delimiter kind and macro operands", "[cleanup][includes]") {
  const std::string source = "#if ! defined (SECOND_GUARD)\n#define SECOND_GUARD 1\n"
      "#define vector <other.hpp>\n#include vector\n#include <vector>\nint value;\n"
      "#include \"vector\"\n#include \"vendor.hpp\"\n#include <vendor.hpp>\n"
      "#include <cassert>\n#include <cassert>\n#include vector\n#endif\n";
  const auto output = cleanup_output(source, true, "SECOND_GUARD");
  CHECK(output == "#if ! defined (SECOND_GUARD)\n#define SECOND_GUARD 1\n"
      "#include <vector>\n#include \"vector\"\n#include \"vendor.hpp\"\n#include <vendor.hpp>\n#include <cassert>\n"
      "#define vector <other.hpp>\n#include vector\nint value;\n#include vector\n#endif\n");
  CHECK(cleanup_output(output, true, "SECOND_GUARD") == output);
}

TEST_CASE("hoisting includes before the outer guard preserves conditional preamble includes", "[cleanup][includes]") {
  const std::string source = "#if PREAMBLE\n#include <vector>\n#endif\n#include <vector>\n"
      "#ifndef PREAMBLE_GUARD\n#define PREAMBLE_GUARD\n#include <string>\n"
      "#if INNER\n#include <string>\n#elifdef OTHER\n#include <only_other.hpp>\n#endif\n#endif\n";
  const auto output = cleanup_output(source, true, "PREAMBLE_GUARD", {"<vector>", "<string>"});
  CHECK(output == "#if PREAMBLE\n#include <vector>\n#endif\n#ifndef PREAMBLE_GUARD\n#define PREAMBLE_GUARD\n"
      "#include <vector>\n#include <string>\n#if INNER\n#elifdef OTHER\n#include <only_other.hpp>\n#endif\n#endif\n");
  CHECK(cleanup_output(output, true, "PREAMBLE_GUARD", {"<vector>", "<string>"}) == output);
}

TEST_CASE("include hoisting preserves comments licenses raw literals and continued directives", "[cleanup][includes]") {
  const std::string source = R"fixture(/* Copyright 2026 Example, license retained. */
%:ifndef EXAMPLE_GUARD
%:define EXAMPLE_GUARD
const char *raw = R"tag(
#include <fake.hpp>


)tag";
#define MULTILINE x \


last
// #include <fake_comment.hpp>
/* prefix license */ #/**/include \
<vector> /* rationale */
%:if FEATURE
%:include <vector> // conditional duplicate rationale
%:include <conditional.hpp>
%:endif
#include <vector> // repeated rationale
#include <path//name.hpp>
%:endif
)fixture";
  const auto output = cleanup_output(source, true, "EXAMPLE_GUARD", {"<vector>"});
  CHECK(output.find("%:define EXAMPLE_GUARD\n#include <vector>\n#include <path//name.hpp>\n") != std::string::npos);
  CHECK(output.find("/* Copyright 2026 Example, license retained. */") != std::string::npos);
  CHECK(output.find("/* prefix license */ /**/ /* rationale */") != std::string::npos);
  CHECK(output.find("// conditional duplicate rationale") != std::string::npos);
  CHECK(output.find("// repeated rationale") != std::string::npos);
  CHECK(output.find("%:include <conditional.hpp>") != std::string::npos);
  CHECK(output.find("R\"tag(\n#include <fake.hpp>\n\n\n)tag\"") != std::string::npos);
  CHECK(output.find("#define MULTILINE x \\\n\n\nlast") != std::string::npos);
  CHECK(output.find("// #include <fake_comment.hpp>") != std::string::npos);
  CHECK(cleanup_output(output, true, "EXAMPLE_GUARD") == output);
}

TEST_CASE("include hoisting rejects absent invalid ambiguous and branched outer guards", "[cleanup][includes][errors]") {
  const std::string valid = "#ifndef A_GUARD\n#define A_GUARD\n#include <vector>\n#endif\n";
  CHECK_THROWS_WITH(cleanup_output(valid, true), Catch::Matchers::ContainsSubstring("--include-guard"));
  CHECK_THROWS_WITH(cleanup_output(valid, true, "A-GUARD"), Catch::Matchers::ContainsSubstring("--include-guard"));
  CHECK_THROWS_WITH(cleanup_output(valid, true, "MISSING"), Catch::Matchers::ContainsSubstring("was not found"));
  CHECK_THROWS_WITH(cleanup_output(valid + valid, true, "A_GUARD"), Catch::Matchers::ContainsSubstring("ambiguous"));
  CHECK_THROWS_WITH(cleanup_output("#ifndef A_GUARD\n#include <vector>\n#endif\n", true, "A_GUARD"),
                    Catch::Matchers::ContainsSubstring("was not found"));
  CHECK_THROWS_WITH(cleanup_output("#ifndef A_GUARD\n#define A_GUARD\n#else\n#endif\n", true, "A_GUARD"),
                    Catch::Matchers::ContainsSubstring("must not have"));
  CHECK_THROWS_WITH(cleanup_output("#ifndef A_GUARD\n#define A_GUARD\n#elifndef OTHER\n#endif\n", true, "A_GUARD"),
                    Catch::Matchers::ContainsSubstring("must not have"));
  CHECK_THROWS_WITH(cleanup_output("#if FEATURE\n" + valid + "#endif\n", true, "A_GUARD"),
                    Catch::Matchers::ContainsSubstring("not outermost"));
  CHECK_THROWS_WITH(cleanup_output("#ifndef A_GUARD\nint code;\n#define A_GUARD\n#endif\n", true, "A_GUARD"),
                    Catch::Matchers::ContainsSubstring("immediately after"));
  CHECK_THROWS_WITH(cleanup_output(valid + "#undef A_GUARD\n", true, "A_GUARD"),
                    Catch::Matchers::ContainsSubstring("one opening #define"));
  CHECK_THROWS_WITH(cleanup_output("#ifndef A_GUARD\n#define A_GUARD\n", true, "A_GUARD"),
                    Catch::Matchers::ContainsSubstring("unterminated"));
  CHECK(cleanup_output(valid, false, "MISSING") == valid);
}

TEST_CASE("mixed guarded includes fail together with assembled lines and guarding directives", "[cleanup][includes][audit]") {
  const std::string source = "#ifndef AUDIT_GUARD\n#define AUDIT_GUARD\n#include <thread>\n#include <alpha>\n"
      "#if FEATURE\n#include <thread>\n#if NESTED\n#include <alpha>\n#endif\n"
      "#else\n#include <thread>\n#endif\n#endif\n";
  try {
    cleanup_output(source, true, "AUDIT_GUARD");
    FAIL("Mixed includes were accepted");
  } catch (const std::runtime_error &error) {
    CHECK(std::string(error.what()) == "include deduplication: mixed guarded and unguarded literal includes:"
        "\n  <alpha>: unguarded assembled lines 4; guarded assembled lines 8 [#if FEATURE (line 5); #if NESTED (line 7)]"
        "\n  <thread>: unguarded assembled lines 3; guarded assembled lines 6 [#if FEATURE (line 5)], 11 [#else (line 10)]"
        "\nUse --allow-mixed-include for each reviewed operand to waive this audit only; those includes still hoist.");
  }
  CHECK_THROWS_WITH(cleanup_output(source, true, "AUDIT_GUARD", {"<unrelated>"}),
                    Catch::Matchers::ContainsSubstring("<thread>: unguarded"));
  const auto waived = cleanup_output(source, true, "AUDIT_GUARD", {"<thread>", "<alpha>"});
  CHECK(waived == "#ifndef AUDIT_GUARD\n#define AUDIT_GUARD\n#include <thread>\n#include <alpha>\n"
      "#if FEATURE\n#if NESTED\n#endif\n#else\n#endif\n#endif\n");
  CHECK(cleanup_output(waived, true, "AUDIT_GUARD") == waived);
  CHECK(cleanup_output(source, false) == source);
}

TEST_CASE("mixed-include audit distinguishes literal identities and ignores only the configured outer guard", "[cleanup][includes][audit]") {
  const std::string source = "#ifndef IDENTITY_GUARD\n#define IDENTITY_GUARD\n#include <thread>\nint value;\n"
      "#include <thread>\n#if FEATURE\n#include \"thread\"\n#include <conditional_only>\n#endif\n#endif\n";
  const auto output = cleanup_output(source, true, "IDENTITY_GUARD");
  CHECK(output.find("#define IDENTITY_GUARD\n#include <thread>\nint value;") != std::string::npos);
  CHECK(output.find("#if FEATURE\n#include \"thread\"\n#include <conditional_only>\n#endif") != std::string::npos);
  const std::string mixed_quote = "#ifndef IDENTITY_GUARD\n#define IDENTITY_GUARD\n#include <thread>\n#include \"thread\"\n"
      "#ifndef VENDOR_GUARD\n#include \"thread\"\n#endif\n#endif\n";
  CHECK_THROWS_WITH(cleanup_output(mixed_quote, true, "IDENTITY_GUARD", {"<thread>"}),
                    Catch::Matchers::ContainsSubstring("\"thread\": unguarded assembled lines 4; guarded assembled lines 6 [#ifndef VENDOR_GUARD (line 5)]"));
  const auto waived = cleanup_output(mixed_quote, true, "IDENTITY_GUARD", {"\"thread\""});
  CHECK(waived == "#ifndef IDENTITY_GUARD\n#define IDENTITY_GUARD\n#include <thread>\n#include \"thread\"\n#ifndef VENDOR_GUARD\n#endif\n#endif\n");
  CHECK_THROWS_WITH(cleanup_output(source, true, "IDENTITY_GUARD", {"thread"}),
                    Catch::Matchers::ContainsSubstring("including delimiters"));
}
