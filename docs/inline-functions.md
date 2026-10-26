# Generator-owned inline definitions

`--inline-functions` (JSON `"inlineFunctions": true`) lets a Clang-enabled
generator add `inline` to ordinary external function definitions. It defaults
off; `--no-inline-functions` overrides configuration. This is independent of
function stitching: unstitched definitions receive the same treatment, and
`--function-stitching off` still supports automatic inlining.

```sh
single_header --config single_header.json --inline-functions \
  --compilation-database build/header-contexts --output dist/library.hpp
```

For config-only generation, set `inlineFunctions`, `functionStitching`, and
`frontendArgs` in JSON. `frontendVariants` adds source-specific alternative
arguments without an externally generated compilation database; see the
[config-only example](../README.md). All relative frontend paths use the config
directory as their working directory.

The input files are not edited. The generator collects physical insertion points
from Clang's function declarations, composes those edits with accepted stitching
edits, and parses the resulting generated header in every supplied context.
The unstitched comparison baseline already includes automatic inlining. Both
residual definitions and definitions moved by stitching therefore participate in
the same final-output validation and byte accounting.

The report's `counts.inlineInsertions` records unique physical definitions that
need an added specifier before stitching. An accepted stitch may replace that
insertion with an in-class definition, so this is not a count of literal `inline`
keywords in the final header. `counts.alreadyInline` remains separate.

In-class definitions, existing inline/constexpr functions, ordinary templates,
and internal-linkage functions already have their relevant linkage/ODR rules and
do not need another specifier. Explicit specializations are ordinary definitions
and may need `inline`. Explicit template-instantiation definitions require a
single owner and are rejected in source inputs. Variables and class static data
definitions are not converted to inline variables. Their ownership remains the
library author's responsibility.

## Compiler contexts and conditional sources

Supply either a compilation database or complete repeated `--frontend-arg`
arguments. With automatic inlining enabled, a database may contain multiple
commands for each source file, representing different preprocessing
configurations. Stitching without automatic inlining retains its one-command-per-
source contract.

Every emitted source declaration must be observed in at least one supplied
configuration. For example, functions under `__EMSCRIPTEN__` and
`!__EMSCRIPTEN__` require contexts covering both branches. Inactive alternatives
inside an observed function body do not need a separate inline insertion: the
specifier on the enclosing definition covers them. Additional configurations
may still be necessary to validate stitching semantics in those alternatives.
An uncovered source declaration produces an error instead of an output with a
silently unannotated definition.

Observed header definitions are also processed. This mode does not establish a
new ODR policy for preexisting, unobserved conditional header definitions; headers
must retain their original multi-translation-unit correctness. It does not prove
arbitrary template instantiations or configurations that were not supplied.

Automatic inlining alone accepts C++11 compiler contexts. Function stitching
continues to require C++17 or later. Unsupported physical declaration ranges or
definitions that cannot legally be inline produce a diagnostic. Macros that
generate entire source declarations require explicit treatment rather than a
guessed insertion location.

In source inputs, a macro invocation outside every observed complete function
definition is rejected, even if it currently expands to nothing: another
configuration could make it generate a definition. Signature attributes and body
macros belong to their observed function's physical range and are supported.
This rule also rejects namespace-level assertion/declaration macros until their
ownership can be established; it does not infer safety from their names.

## Builds and CI

Build the generator with `-DSINGLE_HEADER_WITH_CLANG=ON` and LLVM/Clang 20.
Lightweight release binaries reject automatic-inlining requests clearly; they
continue to support ordinary generation when this option is off.

CI can generate the header once with the Clang-enabled tool, then compile that
exact artifact across its consumer matrix. The composite Action accepts an
`executable` input for such a locally built tool, along with `inline-functions`,
`compilation-database`, and repeated `frontend-arg` inputs.

The C++ `inline` specifier permits matching definitions across translation units;
it does not force the compiler to inline machine code. Verify generated headers
with multiple translation units, including shared function addresses and local
static state, and with the library's supported preprocessing and exception modes.

## csv-parser integration measurement

On October 25, 2026, an isolated snapshot of the working csv-parser headers and
sources included the new `CSV_TRY`/`CSV_CATCH` call sites. The validation copy
removed 99 `CSV_INLINE` annotation uses, its macro definition, and its generator
rewrite. The live csv-parser checkout was not edited by this validation.

The snapshot's explicit mixed-include waiver list was retained (21 identities,
with no `<thread>` waiver). Nine source files were analyzed in four configurations:
native/Emscripten preprocessing branches, each with exceptions on/off. These were
host Clang contexts; defining `__EMSCRIPTEN__` checks that source branch, not a
WebAssembly toolchain or browser runtime.

| Mode | Header bytes | Added inline definitions | Stitched definitions | Analysis time |
| --- | ---: | ---: | ---: | ---: |
| Inline functions, stitching off | 675,433 | 92 | 0 | 187,959 ms |
| Inline functions, stitching all | 669,617 | 92 | 71 | 254,078 ms |

Stitching saves **5,816 bytes against the matching automatically inlined baseline**.
It accepts four free functions and 67 methods, including `CSVReader::read_csv`
with its contained try/catch macros. The other 21 candidates retain their
definitions, with automatic inline insertion where required. The count of 92
insertions describes the original definitions before accepted stitches replace
some of them with in-class definitions.

The stitched run made 36 discovery parses, 36 baseline parses, 39 trial parses,
and 36 fresh final parses, with one diagnostic-guided partition and one cached
batch reuse. Input manifests, both reports, and header hashes are recorded in the
ignored local `build/validation/csv-inline-owned/metrics.json`. These timings are
local measurements, not general performance guarantees. The historical cleanup
measurements in the stitching documentation use an older input snapshot and
must not be subtracted from these sizes.

Both generated headers compiled and passed the same four runtime groups with
MSVC 19.51 and Clang 20.1.8: a two-translation-unit 500,000-row mmap/stream check
(46,619,067 bytes), exception-enabled and compiler-disabled-exception smoke
tests, and the pinned upstream field/format/row/iterator suite (56 cases,
87,097 assertions). These validate the supplied host configurations and tested
behaviors; they do not claim universal macro or target coverage.
