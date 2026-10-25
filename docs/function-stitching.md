# Conservative function stitching

The optional LLVM/Clang 20 backend moves supported source definitions to existing
header declarations. `--function-stitching off|free|methods|all` defaults to `off`.
The default build and release binaries retain the lightweight generator; requesting
stitching from those binaries fails with build instructions. Explicit `off`
overrides JSON configuration and follows the original generation path.

Both generation paths automatically collapse repeated ordinary blank lines and
remove Doxygen file directives' command/optional filename. Literal/comment
payloads, continued directives, licenses, and Doxygen code/verbatim/fenced
examples remain protected.

Include hoisting and deduplication is a separate option, off by default. Set
`--deduplicate-includes --include-guard MYLIB_HPP` (JSON `deduplicateIncludes: true`
and `includeGuard: "MYLIB_HPP"`); the name is required configuration, with no
hardcoded guard. After stitching edits and collation assemble the text, the pass
first audits all literal includes for mixed guarded and unguarded occurrences,
ignoring only the configured outer guard. Every conflict is reported together,
with assembled line numbers and guarding directives. An error leaves existing
output/report files untouched and aborts before compiler discovery. Explicit
repeatable `--allow-mixed-include '<thread>'` waivers (JSON `allowMixedIncludes`
string array, Action multiline `allow-mixed-include`) suppress only that audit
identity: they never exclude a header from hoisting. CLI waivers replace the JSON
list; delimiter spelling distinguishes angle from quoted operands. With the audit
satisfied, the pass gathers every unguarded literal angle/quoted include, treating only that outer
guard as transparent, and emits a first-seen-order block immediately after its
opening `#define`. Code, namespaces, and unrelated macros do not limit the scan.
It removes later occurrences of those same literal includes, even in conditional
branches; distinct conditional-only includes and macro-expanded operands stay in
place. Angle and quote operands are distinct. Comments remain at their original
locations. Missing, ambiguous, malformed, or alternatively branched outer guards
fail clearly. `--no-deduplicate-includes` overrides configured enablement.

This explicit reordering assumes dependencies suitable for inclusion before their
original code, scope, and macro settings; it is not a compiler-equivalence claim.
See the [README](../README.md) for complete include-pass behavior. Each unstitched
baseline, stitched trial, and final render uses the same optional audit/include pass,
automatic metadata cleanup, and final blank-line collapse. Consequently compiler
checks and report sizes describe the exact bytes written, including postprocessing.

The implementation lives in `src/stitching/`: `engine.cpp` coordinates discovery,
generated baselines, trial batches and reports; `frontend.cpp` owns Clang parsing
and candidate collection; `semantics.cpp` fingerprints types, identities and ASTs;
`source.cpp` handles physical ranges, directives and paired edits; and
`validation.cpp` compares recorded artifact semantics. Internal headers define
their interfaces and shared model. `clang_sdk_allocator.hpp` isolates SDK allocator
compatibility and is included before Clang/LLVM headers in each frontend-facing
translation unit. The public entry point remains in `src/stitching.hpp`.

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
methods, nonvirtual constructors/destructors, operators and conversions of
non-template classes. Useful supported bodies include local variables,
branches and loops, references and aliases, direct/member/operator calls, string
and vector construction/assignment, RAII temporaries and destruction, casts,
`sizeof`, `noexcept`, recursion, non-generic lambdas with captures, new/delete,
rewritten comparisons, and shared function-local static state. Constructor
initializer lists move with the body; explicit and implicit initializer semantics
and implicit destruction dependencies are recorded. Header
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
Feature-conditional definitions can move when the declaration and definition have
identical outer branch stacks, including each preceding `else`/`elif` branch.
Canonical whole-file guards are excluded from that comparison. Body-contained
conditional branches move intact, including inactive source text. Clang's raw
lexer distinguishes directives from raw strings and comments. State-changing
directives and `_Pragma`/`__pragma` anywhere inside a source definition are
also skipped, preventing moved macro state from activating untouched header branches.
Diagnostic and region pragmas are allowed; other physical pragmas or `#line`
directives conservatively disable relocation for the scanned set.

Signature macros are skipped. To explicitly allow an object macro whose expansion
is empty or exactly `inline`, use `--inline-annotation CSV_INLINE` (repeatable).
Its full source spelling is removed with the definition, including an empty prefix
that is absent from the AST range. Rewrites for configured annotations must also
be empty or `inline`. Simple object body macros are supported when expanded-token
and semantic records remain equal. Function-like body macros, pragma-producing
macros and macro-generated declarations remain deferred.

Deferred candidates include templates and class-template methods, constructor
function-try-blocks, defaulted/deleted definitions, global allocation/deallocation
replacements, virtual methods, constexpr/consteval functions,
C linkage, variadics, most attributes, indirect calls, generic lambdas, dependent
expressions, unsupported AST nodes, and internal-linkage dependencies. Dependencies
on ordinary library constructors/operators/templates are analyzed by Clang and
are distinct from relocating their definitions. Inherited `nodiscard` is retained
from the header declaration; source-only attributes still reject relocation.

## Validation boundary and diagnostics

The frontend parses the original TUs for discovery and classification, then saves
semantic records for the exact unstitched generated artifact under every supplied
source context. This generated artifact is the baseline for every tentative and
final stitched artifact. Destination parses replace the main source once with an
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
their original file and byte offset; ambiguous provenance fails comparison. The
complete original provenance index is collected before any generated baseline is
recorded, so every supplied context uses the same identity mapping. Locals
and parameters use corresponding traversal ordinals rather than AST addresses.
Residual non-type template parameters use their depth, index, canonical type and
pack status within the corresponding function record; templates remain deferred
as relocation candidates.
Residual closure types and their embedded template-argument USRs use the enclosing
function identity plus lexical closure ordinal, retaining selected-specialization
comparison when inline distribution changes Clang's closure-linkage spelling.
The same normalization applies to declaration types, including deduced return
types containing a closure, without dropping the canonical type comparison.
Function-template specialization identities contain the selected primary template
and structured canonical arguments; this avoids context-sensitive USR compression
without discarding overload or argument identity. Anonymous enum types use their
source-independent compiler declaration USRs when available; location-based or
ambiguous identities remain a conservative limitation.

Unstitched generated bodies and declaration records are compared for each trial
batch, including residual code, and the final accepted batch is freshly checked
again. Comparison checks both directions: added and missing recorded definitions
or declarations also reject relocation.
Unknown constructs prevent candidate acceptance. Delayed, uninstantiated template
bodies do not supply a semantic record and remain outside the transformation
guarantee. This is a bounded source-relocation check for its declared vocabulary,
not a proof of arbitrary C++ equivalence or future template instantiations.
Each moved function must stay within the supported vocabulary and internal-linkage
policy in every supplied generated baseline and trial context, including alternate
active body branches. Unsupported unchanged residual bodies do not veto neighboring
edits.

Original-to-unstitched differences are reported separately in `baselineDifferences`
as informational diagnostics. They describe preexisting collation behavior and
cannot veto relocation. If the unstitched artifact fails parsing, the report marks
`baseline` as `invalid`, records its compiler diagnostics and
`destination_parse_error` reason, and blocks relocation with `baseline_invalid`.
The unchanged legacy output does not carry a semantic safety claim. A successful
delta check covers the supplied configurations only. Multiple commands
for a source are deliberately rejected; validate separate builds/configurations
explicitly. Consumer macros, different compiler semantics, unmodeled extensions,
and uninstantiated templates are not universally proven.

Diagnostics go to stderr. The optional version-1 JSON report records contexts,
locations, accepted/skipped reasons and compiler diagnostics, matching and removal
counts, functions already inline, elapsed analysis time, retained implementation
state, and `bytes` (`unstitched`, `stitched`, signed `savings`). Original-to-unstitched
diagnostics are deduplicated by reason and bounded to ten records. The report must
be outside scanned roots and distinct from inputs, config,
database and output, including existing filesystem aliases. Remaining definitions
and source globals can still require a one-TU implementation section. Stitching does
not promise faster code, stable binary ABI, or a wholly header-only result.

Eligible candidates are sorted by identity and tested together. A failing batch
first uses exact diagnostic locations or changed-record identities to partition
suspected edits. Unattributed failures split deterministically in halves; each
partition is checked with the previously
accepted edits against the unchanged generated baseline. Rejected singletons stay
in the legacy source section. A successful batch needs one frontend pass per source
context; failures incur subdivision passes. All supplied contexts are retained,
including filename-sensitive contexts. The exact final artifact is parsed again
under every context, bypassing the trial cache.

Shared sources of truth are scoped to one run: frozen source bytes and provenance,
one physical token/directive index per file, and validation results keyed by the
complete canonical edit set. Each frontend parse owns its FileID identity cache,
canonical type spelling cache, and per-file preprocessing event indexes. Offset
queries restore preprocessing encounter order, including repeated inclusions.
AST pointer keys never survive their owning parse. Changed source bytes abort
emission; no mutable cache is shared across invocations. `work` records actual
parse counts and cache activity; `phaseMilliseconds` separates discovery, baseline,
trials and fresh final validation. Run large analyses with an external process
deadline; in-process frontend cancellation is not implemented.

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

That sanitizer job links the distribution's unsanitized LLVM SDK and sets
`SINGLE_HEADER_CLANG_UNSANITIZED_SDK=ON`. LLVM 20's inline arena allocator otherwise
mixes client-side poisoning with SDK allocations that do not unpoison the arena,
causing a startup use-after-poison report. The compatibility header matches only
LLVM's manual arena annotations to the SDK. Source ASan/UBSan instrumentation,
heap bounds and use-after-free checks remain enabled; arena suballocation redzones
require an LLVM SDK built with matching sanitizers. Leave the option OFF with a
sanitized SDK. No runtime sanitizer suppressions are used.

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

Before automatic output cleanup, the expanded support and shared indexes relocated
69 of 91 definitions (one free function and 68 methods), reducing the generated
header from 680,603 to 675,072 bytes (5,531 bytes saved). Analysis took 62,734 ms
(63,386 ms external wall time), compared with 1,791,489 ms for the earlier
30-definition sequential run on the same corpus and contexts: approximately
28.6 times faster. These are local measurements, not general performance claims.
The final run made nine discovery parses, nine baseline parses, ten trial parses
and nine fresh final parses. One diagnostic-guided partition isolated a relocation
that needed a source-only helper; canonical edit-set caching reused its failed
batch result. The report recorded 1,014,596 canonical-type cache hits.

The replacement opt-in include-hoisting pass was measured before the mixed-guard
audit was added. Using `--deduplicate-includes --include-guard CSV_HPP`, the same
nine-context run relocated all 69 definitions and produced a 670,773-byte header
from a matching 676,641-byte unstitched baseline (5,868 bytes saved by stitching).
Include hoisting saved 3,727 bytes against the 680,368-byte default unstitched
output, reduced literal include directives from 253 to 63, and placed 33 unique
headers after the configured guard. `<algorithm>`, `<vector>` and `<string>` each
appeared once. Analysis took 64,707 ms (64,791 ms external wall time), with the
same 37 frontend parses and skip reasons. Timings vary between local runs.

The current strict audit intentionally rejects that upstream revision before
Clang discovery or output writing: 22 headers have both guarded and unguarded
occurrences. In particular, `data_frame_executor.hpp` includes `<thread>` without
the `CSV_ENABLE_THREADS` guard used by the scheduler and task pool. This is tracked
in [csv-parser #346](https://github.com/vincentlaucsb/csv-parser/issues/346).
A local, ignored validation config waives only `<thread>` for development; the
other 21 diagnostics remain errors. No upstream configuration or production
exception list was changed. The preceding size/runtime measurements describe the
preserved pre-audit artifact, not a successful strict-audit generation. Generating
it under the current policy requires resolving the remaining conflicts or
explicitly reviewing and waiving each one.

MSVC and Clang consumers also passed the upstream 56-case suite (87,097
assertions), the 500,000-row dual-path runtime check, and exception-enabled and
compiler-disabled-exception smoke tests on the preserved hoisted artifact. Its
matching unstitched control passed the same four runtime groups. The remaining
skips retain their source implementations and explicit reasons in the report.

The original-to-unstitched report identifies a preexisting collation difference:
the `CSV_NON_NULL(2)`
attribute on the source definition of the `CSVRow::iterator` constructor becomes
visible in TUs that originally saw an unannotated header declaration. This is an
informational diagnostic. Relocation compares against the unstitched generated
artifact, preserving its attribute records while permitting safe neighboring
functions to move. The report measures actual accepted definitions and byte savings;
see the PR validation notes for the completed corpus run.

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
