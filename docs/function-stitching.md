# Conservative function stitching

The optional LLVM/Clang 20 backend moves supported source definitions to existing
header declarations. `--function-stitching off|free|methods|all` defaults to `off`.
The default build and release binaries retain the lightweight generator; requesting
stitching from those binaries fails with build instructions. Explicit `off`
overrides JSON configuration and follows the original generation path.

## Build and compilation context

Install LLVM **20** development headers and libraries, including Clang LibTooling,
then configure a separate build:

```sh
cmake -S . -B build/clang -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DSINGLE_HEADER_WITH_CLANG=ON \
  -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm \
  -DClang_DIR=/path/to/llvm/lib/cmake/clang
cmake --build build/clang --parallel 2
ctest --test-dir build/clang --output-on-failure --parallel 2
```

The enabled executable uses the configured LLVM installation's `clang++` path to
locate driver resources for explicit arguments. Keep that installation available.
It is a local development build, not a standalone portable release package.
Compilation-database commands retain their own executable paths and working
directories. Compiler resource headers and the platform C++ standard library must
be installed and visible. On Windows, run in a Visual Studio developer environment;
the official LLVM 20.1.8 Windows development libraries require the static MSVC
runtime (`-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` for a Release build). Some
Windows LLVM packages export a stale absolute `diaguids.lib` dependency; correct
that package's local CMake export to the installed Visual Studio DIA SDK library.
The backend reserves a 32 MiB Windows stack for Clang template analysis.

Supply exactly one compilation command per scanned source:

```sh
build/clang/single_header --include-root include --source 'src/*.cpp' \
  --output dist/library.hpp --function-stitching all \
  --compilation-database build/analysis --stitching-report build/stitching.json
```

The database option accepts a directory or `compile_commands.json`. Missing or
duplicate commands are errors. Generate the database with Ninja/Makefiles, for
example with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`. Prefer Clang/clang-cl commands;
incompatible driver flags and missing system/generated headers cause an original
analysis error rather than guessed semantics. ClangTool removes compile/output
actions for frontend analysis; target, language, include paths, working directory,
defines, undefines, forced includes, and semantic flags remain per source.

Alternatively provide complete arguments, one option per occurrence. These apply
to every scanned source and resolve relative paths from the invocation directory:

```sh
build/clang/single_header --include-root include --output dist/library.hpp \
  --function-stitching free --frontend-arg -std=c++17 \
  --frontend-arg -Iinclude --frontend-arg -DMY_CONFIGURATION=1
```

JSON keys are `functionStitching`, `compilationDatabase`, `frontendArgs`,
`stitchingReport`, and `inlineAnnotations`. Database/report paths resolve relative
to the config file; strings inside `frontendArgs` remain compiler arguments.
Explicit CLI values override the corresponding config field. The Action forwards
the same options only when provided. Use its `executable` input with your locally
built enabled CLI; standard downloaded release binaries do not include Clang.

## Supported transformation

Candidates are ordinary non-template external free functions and ordinary/static
methods of non-template classes. Useful supported bodies include local variables,
branches and loops, references and aliases, direct/member/operator calls, string
and vector construction/assignment, RAII temporaries and destruction, casts,
`sizeof`, `noexcept`, recursion, and shared function-local static state. Header
declarations keep their defaults, suffix qualifiers, access position and comments;
AST token ranges supply definition parameter names. Simple unnamed declaration
parameters are supported. Repeated declarations remain, with the last available
declaration selected as the insertion site. This can permit mutual recursion when
earlier declarations establish both names.

Free definitions receive `inline` unless already specified. In-class definitions
use implicit inline. Definition removal and declaration replacement are paired
edits against frozen original input bytes. The existing collation pipeline renders
each tentative batch, including include stripping, macro rewrites and implementation
marker insertion. A failed relocation retains both original pieces. Source-only
dependencies are observed, never hoisted or repaired.

Canonical whole-file `#ifndef`/`#define` guards and `#pragma once` are retained.
Feature-condition candidates, including conditionals anywhere inside a body, are
skipped. Clang's raw lexer distinguishes directives from raw strings and comments.
Other directives and `_Pragma`/`__pragma` anywhere inside a source definition are
also skipped, preventing moved macro state from activating untouched header branches.
Diagnostic and region pragmas are allowed; other physical pragmas or `#line`
directives conservatively disable relocation for the scanned set.

Signature macros are skipped. To explicitly allow an object macro whose expansion
is empty or exactly `inline`, use `--inline-annotation CSV_INLINE` (repeatable).
Its full source spelling is removed with the definition, including an empty prefix
that is absent from the AST range. Rewrites for configured annotations must also
be empty or `inline`. Macro-generated declarations and body macros remain skips.

Deferred candidates include templates and class-template methods, special members,
operators and conversion functions, virtual methods, constexpr/consteval functions,
C linkage, variadics, attributes, indirect calls, lambdas, new/delete, dependent
expressions, unsupported AST nodes, and internal-linkage dependencies. Dependencies
on ordinary library constructors/operators/templates are analyzed by Clang and
are distinct from relocating their definitions.

## Validation boundary and diagnostics

The frontend parses the original TUs and the exact generated artifact under every
supplied source context. Destination parses replace the main source once with an
include of an in-memory artifact. A preprocessor entry assertion verifies that the
artifact was actually analyzed. No `#line` directives or preprocessed output are
inserted to mask source-location changes. Error recovery ASTs are never inspected.

Semantic records compare node structure; declaration and local/parameter identities;
selected callees, constructors and destructors; canonical expression types, value
categories, cast/operator kinds, constants, effective floating-point options,
defaults used at calls, declaration attributes, global/member initializers and
named class layouts. Expanded body-token records detect destination macro changes,
including identifiers that only become macros after movement. Internal declarations
that remain in place use unique kind/name/type/raw-token provenance to map back to
their original file and byte offset; ambiguous provenance fails comparison. Locals
and parameters use corresponding traversal ordinals rather than AST addresses.
Residual non-type template parameters use their depth, index, canonical type and
pack status within the corresponding function record; templates remain deferred
as relocation candidates.
Residual closure types and their embedded template-argument USRs use the enclosing
function identity plus lexical closure ordinal, retaining selected-specialization
comparison when inline distribution changes Clang's closure-linkage spelling.
Function-template specialization identities contain the selected primary template
and structured canonical arguments; this avoids context-sensitive USR compression
without discarding overload or argument identity. Anonymous enum types use their
source-independent compiler declaration USRs when available; location-based or
ambiguous identities remain a conservative limitation.

Original mapped bodies and declaration records are compared after each accepted
edit, including residual code, and the final accepted batch is checked again.
Unknown constructs prevent candidate acceptance. Delayed, uninstantiated template
bodies do not supply a semantic record and remain outside the transformation
guarantee. This is a bounded source-relocation check for its declared vocabulary,
not a proof of arbitrary C++ equivalence or future template instantiations.

If the unchanged legacy artifact fails parsing or comparison, the report records a
separate `baseline_invalid` limitation and returns unchanged legacy output. This is
not an original-source analysis failure and does not claim the legacy artifact is
safe. A successful check covers the supplied configurations only. Multiple commands
for a source are deliberately rejected; validate separate builds/configurations
explicitly. Consumer macros, different compiler semantics, unmodeled extensions,
and uninstantiated templates are not universally proven.

Diagnostics go to stderr. The optional version-1 JSON report records contexts,
locations, accepted/skipped reasons and compiler diagnostics, matching and removal
counts, functions already inline, elapsed analysis time, and retained implementation
state. The report must be outside scanned roots and distinct from inputs, config,
database and output, including existing filesystem aliases. Remaining definitions
and source globals can still require a one-TU implementation section. Stitching does
not promise faster code, stable binary ABI, or a wholly header-only result.

Candidates are tested deterministically in identity order. Every accepted prefix
is revalidated before another candidate is added. Rejected candidates stay in the
legacy source section. This is deliberately conservative and order dependent; it
does not search all alternative placements or repair inseparable groups. Cost is
approximately one full frontend pass per candidate per source context, plus original,
baseline and final passes. Run large analyses with an external process deadline;
in-process frontend cancellation is not implemented.

## Tests and local validation

The enabled suite covers useful standard-library bodies, aliases, named/unnamed
parameters, defaults and method qualifiers, later member lookup, repeated declarations,
direct/mutual recursion, multi-TU link/runtime checks, shared addresses/local state,
silent overload changes, macro/conditional/guard fidelity, annotation policies,
source-only dependencies, missing/ambiguous compilation contexts, report safety,
stdout separation, and exact fallback. Subprocesses have 180-second deadlines and
per-process work directories. The normal OFF matrix continues to run existing
goldens and the original compilation check. Enabled Linux CI exercises GCC and
Clang consumer builds with the same Clang 20 semantic backend, plus a Clang
ASan/UBSan build of the analysis/edit code.

On Windows/MSVC 19.51 with LLVM 20.1.8, the enabled semantic/link/runtime suite and
the independent lightweight build were run locally during implementation. See the
PR validation notes for final counts and the pinned latest csv-parser integration
revision/results; generated files, toolchains and consumer build directories remain
under ignored `build/`.

The local Release executable measured approximately 48 MB with the static Clang
backend and 0.31 MB without it; the enabled build used separate PDB debug
information. These are local build measurements, not published
package sizes or startup/memory guarantees.

### Latest csv-parser check

The locally checked upstream revision was
`e623089b34e6842fca909d1d0af04ef1b93f423d` (October 25, 2026). Its C++ sources and
build configuration are unchanged from `b3049bf296d92001a49aa546944462702382428b`,
which supplied the analysis inputs. The original library and generated artifact
were compiled and linked locally with MSVC 19.51 and Clang 20.1.8. Two-TU consumers
also parsed 500,000 rows through both mmap and stream paths, including distinct
quoted fields and embedded newlines (46,619,067 bytes).

For this configuration, all mode retains all 91 source definitions. The report
identifies a genuine preexisting legacy-collation limitation: the `CSV_NON_NULL(2)`
attribute on the source definition of the `CSVRow::iterator` constructor becomes
visible in TUs that originally saw an unannotated header declaration. The backend
preserves attribute comparison and returns unchanged legacy output with an explicit
baseline limitation. This corpus establishes compilation and safe fallback; it
does not establish CSV relocation coverage. The accepted string/vector/method and
multi-TU runtime cases in the repository tests exercise actual relocation separately.

The analysis invocation was equivalent to:

```sh
single_header --config path/to/csv-parser/single_header.json \
  --output build/csv-stitched/csv.hpp --function-stitching all \
  --inline-annotation CSV_INLINE --compilation-database build/csv-context \
  --stitching-report build/csv-stitched/report.json
```

`build/csv-context/compile_commands.json` selected the nine `csv` target commands
from a Clang Release Ninja build (`CSV_BUILD_TESTS=OFF`, `CSV_BUILD_PROGRAMS=OFF`,
`CMAKE_EXPORT_COMPILE_COMMANDS=ON`). Other targets/configurations were excluded
deliberately so each analyzed source had exactly one command.
