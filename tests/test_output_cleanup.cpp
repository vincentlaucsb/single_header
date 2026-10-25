#include "test_support.hpp"
#include "../src/output_cleanup.hpp"

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

TEST_CASE("stdlib dedup retains positions and respects macro conditional and scope barriers", "[cleanup]") {
  const std::string source = R"fixture(#include <vector>
int a;
struct Box { int value; };
#include <vector>
#define MODE 1
#include <vector>
#undef MODE
#include <vector>
#if FEATURE
#include <vector>
#else
#include <vector>
#endif
#include <vector>
namespace nested {
#include <vector>
}
#include <vector>
#include <vector>
#include <cassert>
#include <cassert>
#include <assert.h>
#include <assert.h>
#include <vendor.hpp>
#include <vendor.hpp>
#include <vector> // rationale must survive
#include <vector> // rationale must survive
#pragma pack(push, 1)
#include <vector>
)fixture";
  const auto expected = R"fixture(#include <vector>
int a;
struct Box { int value; };
#include <vector>
#define MODE 1
#include <vector>
#undef MODE
#include <vector>
#if FEATURE
#include <vector>
#else
#include <vector>
#endif
#include <vector>
namespace nested {
#include <vector>
}
#include <vector>
#include <cassert>
#include <cassert>
#include <assert.h>
#include <assert.h>
#include <vendor.hpp>
#include <vendor.hpp>
#include <vector> // rationale must survive
#include <vector> // rationale must survive
#pragma pack(push, 1)
#include <vector>
)fixture";
  CHECK(cleanup_output(source) == expected);
  const std::string fake = "const char *raw = R\"tag(\n#include <vector>\n)tag\";\n"
                           "// #include <vector>\n#include <vector>\n#include <vector>\n";
  CHECK(cleanup_output(fake) == fake.substr(0, fake.rfind("#include <vector>")));
  CHECK(cleanup_output("#include <vector>\n#include \\\n<vector>\n#include <vector>\n") ==
        "#include <vector>\n#include \\\n<vector>\n#include <vector>\n");
  const auto exposed = cleanup_output("#include <vector>\n\n#include <vector>\n\nint value;\n");
  CHECK(exposed == "#include <vector>\n\nint value;\n");
  CHECK(cleanup_output(exposed) == exposed);
  CHECK(cleanup_output(cleanup_output(source)) == cleanup_output(source));
}

TEST_CASE("stdlib dedup stays within one guarded branch and honors pragma operators and digraphs", "[cleanup]") {
  const std::string guarded = "#if FEATURE\n#include <vector>\n#include <vector>\n#else\n#include <vector>\n#include <vector>\n#endif\n"
      "#include <vector>\n#include <vector>\n#if FEATURE\n#include <vector>\n#endif\n";
  CHECK(cleanup_output(guarded) == "#if FEATURE\n#include <vector>\n#else\n#include <vector>\n#endif\n"
      "#include <vector>\n#if FEATURE\n#include <vector>\n#endif\n");
  const std::string barriers = "#include <vector>\n_Pragma(\"pop_macro(\\\"MODE\\\")\")\n#include <vector>\n"
      "__pragma(pop_macro(\"MODE\"))\n#include <vector>\n%:define MODE 1\n#include <vector>\n"
      "%:if FEATURE\n#include <vector>\n%:else\n#include <vector>\n%:endif\n#include <vector>\n"
      "namespace nested <%\n#include <vector>\n%>\n#include <vector>\n";
  CHECK(cleanup_output(barriers) == barriers);
  const std::string continued = "%:define BLOCK first \\\n\n\nlast\n";
  CHECK(cleanup_output(continued) == continued);
  const std::string wrapped = "#/**/define RESTORE _Pragma(\"pop_macro(\\\"GUARD\\\")\")\n"
      "#include <vector>\nRESTORE\n#include <vector>\n#include <vector>\n";
  CHECK(cleanup_output(wrapped) == wrapped.substr(0, wrapped.rfind("#include <vector>")));
  const std::string pasted = "#define JOIN(a,b) a##b\n#define RESTORE JOIN(_Pr,agma)(\"pop_macro(\\\"GUARD\\\")\")\n"
      "#include <vector>\nRESTORE\n#include <vector>\n";
  CHECK(cleanup_output(pasted) == pasted);
  const std::string external = "#include <vector>\nEXTERNAL_PRAGMA_MACRO\n#include <vector>\n";
  CHECK(cleanup_output(external) == external);
  const std::string literal = "#include <vector>\n\"ordinary literal\"\n#include <vector>\n";
  CHECK(cleanup_output(literal) == literal);
}
