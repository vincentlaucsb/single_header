# Compiler features for function stitching

Revised review and implementation plan for [issue 1, free functions](https://github.com/vincentlaucsb/single_header/issues/1) and [issue 2, class methods](https://github.com/vincentlaucsb/single_header/issues/2). Repository implementation inspected at `56fd9f9`; analysis updated October 25, 2026.

Both proposals are viable as restricted source transformations backed by compiler frontend analysis. The useful boundary is to let a mature frontend understand ordinary C++, while allowing this tool to relocate only constructs for which it implements explicit checks. A tokenizer is one component, not the semantic limit of the design.

Recommend an optional Clang LibTooling backend. Build shared analysis for both issues, release free-function stitching first to validate the pipeline, and add ordinary non-template methods immediately afterward. Class methods do not require an independent compiler subsystem. Retain the existing lightweight generator and its unchanged default behavior.

The main benefit is automated placement and reduced annotation work in generated headers. This does not guarantee faster code, shorter compilation, binary ABI compatibility, or that partially stitched output becomes entirely header-only. In particular, code already emitted with `CSV_INLINE=inline` is already addressing linkage without moving bodies.

## Necessary compiler features and their usefulness

These are design assessments, not measured implementation costs or coverage percentages. Integration effort assumes reuse of Clang; writing equivalent C++ semantics from scratch would be substantially more work.

| Capability | Why stitching needs it | Useful code it enables | Priority and integration effort |
| --- | --- | --- | --- |
| Compilation context | Meaning depends on language mode, target, include search, command-line macros, forced includes, and compiler options. | Real libraries using platform headers and configured features. | Required first; moderate configuration and portability work. |
| Lexer and source manager | Locate physical source text, distinguish spelling from macro expansion, and preserve comments and exact edit boundaries. | Multiline declarations, raw strings, comments, qualified definitions. | Required; moderate edit engineering. |
| Preprocessor provenance | Know which tokens came from macros, which regions were conditional, and which includes/directives established context. | Accurate skips initially; later, known annotation macros and recognized header guards. | Required; moderate initially, substantial for portable conditional relocation. |
| Parser and declaration AST | Identify real functions, bodies, namespace/class ownership, declarators, defaults, and qualifiers. | Ordinary free functions and methods, including pointers/references and class types. | Required; low-to-moderate extraction work after frontend setup. |
| Declaration identity and types | Connect redeclarations and definitions semantically rather than by spelling. Track linkage and canonical types. | Overloads, typedefs, namespace aliases, repeated declarations, different parameter names. | Required; moderate indexing and identity-mapping work. |
| Name lookup and expression semantics | Determine referenced declarations, selected overloads, implicit conversions, member access, and expression types. | Wrappers, helper calls, container operations, getters/setters, ordinary algorithm bodies. | Required for useful coverage; substantial validation work. |
| Destination reanalysis | Establish what the same body means where it will actually be emitted. Original AST information alone cannot establish this. | Safe movement despite namespace/class scope changes; rejection of silent overload changes. | Required; substantial, central custom component. |
| Dependency availability and type completeness | Distinguish a dependency visible at the new location from one that only appears later or in the source file. | Calls to existing header APIs; clear explanations for source-only helpers and pimpl failures. | Required; moderate with compiler diagnostics and semantic records. |
| Linkage and ODR checks | Adding inline does not make references to different TU-local entities equivalent. | Reliable multi-TU output for supported functions, including shared function-local state. | Required; moderate rule implementation and substantial testing. |
| Template and constant-expression analysis in dependencies | A non-template function can use templates, overloaded operators, implicit constructors/destructors, sizeof, and noexcept. | std::string, std::vector, std::unique_ptr, RAII, typed wrappers. | High value early; use the frontend, restrict what the validator accepts. |
| Class scope, access, and member semantics | Resolve this, fields, member overloads, nested types, and completion of class scope. | Issue 2's ordinary methods and static methods. | High value; modest incremental effort after the shared semantic validator. |
| Include/declaration relocation | Repair dependencies that are not available at the insertion point. | More implementation-heavy functions whose bodies need source-only includes/helpers. | Optional; high risk and substantial extra work. Defer. |
| Generic dependent-template relocation | Preserve two-phase lookup and future instantiation behavior when moving templates themselves. | Template definitions and methods of class templates. | Defer; much larger proof obligation than parsing template dependencies. |
| Optimizer, IR generation, assembler, production linker | These do not answer source binding or edit-range questions. | No necessary improvement to relocation decisions. | Not needed in the generator. Compilers/linkers remain necessary in validation tests. |

Clang's AST retains source-oriented structure together with semantic nodes such as implicit casts and declaration references, which is a useful basis for this analysis. [Clang AST introduction](https://clang.llvm.org/docs/IntroductionToTheClangAST.html).

### The most important scope distinction

Skipping template definitions as transformation candidates does not mean a frontend can ignore templates. For example:

```cpp
int count() {
    std::vector<int> rows{1, 2, 3};
    return static_cast<int>(rows.size());
}
```

The candidate is non-template, but analyzing it involves a class-template specialization, constructors, member calls, and destruction. Similarly, moving a method that assigns a std::vector involves an overloaded operator even if operator definitions themselves are excluded from stitching.

A reusable full frontend with a restricted transformation policy is therefore a much more practical subset of the compiler toolchain than a homemade frontend with a tiny accepted language. The tool does not need to optimize, generate machine code, or inspect the implementations of every external callee to know whether the same call remains selected. It does need to account for implicit operations and reject semantic node kinds it cannot compare.

### What semantic validation must compare

This original program illustrates the requirement:

```cpp
// api.hpp
int pick(long);
int run();
int pick(int);

// api.cpp, after including api.hpp
int pick(long) { return 1; }
int pick(int) { return 2; }
int run() { return pick(0); }
```

Replacing the declaration of run with its body changes its result from 2 to 1 while leaving valid C++. This is a test for the compiler-backed design, not an argument against it. The frontend identifies the selected overload in both locations; the stitcher rejects the edit when they differ. [C++ working draft on name lookup](https://eel.is/c++draft/basic.lookup.unqual).

For accepted AST constructs, collect and compare:

- References to declarations, including types, members, aliases, and variables.
- Selected direct calls, member/operator calls, constructors, destructors, and conversion functions.
- Expression types, value categories, implicit conversions, default arguments used at calls, and constant-expression results where they affect semantics.
- Local/parameter bindings, lifetime-relevant temporary construction and cleanup, and relevant attributes or pragma state.
- Expanded tokens for the moved code, allowing only specifically documented changes such as adding inline. Checking only macros that expanded originally misses identifiers that first become macros at the destination.

Clang exposes canonical function declarations and selected direct callees; these provide useful inputs, not a complete transformation validator. [FunctionDecl](https://clang.llvm.org/doxygen/classclang_1_1FunctionDecl.html), [CallExpr](https://clang.llvm.org/doxygen/classclang_1_1CallExpr.html).

Do not compare AST pointer addresses or raw AST dumps across parses. Use original-source identities and an edit source map for moved declarations; map parameters by ordinal and locals by corresponding source nodes. Cross-TU identities need qualified ownership, type/linkage information, and original declaration provenance. Compiler USRs may help with named entities but do not replace this mapping for locals or anonymous entities.

An equal record is evidence within a deliberately supported semantic vocabulary and configuration. It is not a universal C++ equivalence proof. Unknown constructs, changes in relevant attributes, unmodeled compiler state, or unresolved identities cause a skip. Keep tests for effects outside the moved body as well: all mapped declarations and bodies affected by the resulting artifact must retain their intended semantics, not merely the candidate itself.

## Expected practical value

The inspected examples suggest the following priorities. These are source-based assessments; no prototype has yet measured acceptance rates.

| Example | What analysis buys | Remaining obstacle or benefit |
| --- | --- | --- |
| `tests/fixtures/simple/include/simple/api.cpp:5`, simple::name | Understand std::string construction and match the declaration despite a class return type. | The header already includes string. A useful early positive target, subject to actual reanalysis. |
| `tests/fixtures/csv_style/include/csv/internal/row.cpp:5`, csv::parsed_rows | Understand vector construction, a member call, and a cast. | vector is included only in the source, after the intended header insertion point. First release should diagnose a missing dependency and retain the body, not hoist the include automatically. |
| `src/helpers.cpp`, normalize_path | Match the filesystem types despite the fs alias. | Its call to a helper in an anonymous namespace prevents straightforward movement into the public header. Semantic analysis makes the rejection precise. |
| Neighboring CSV parser, `include/internal/csv_format.cpp:92`, CSVFormat::column_names | Resolve fields, vector assignment, reference return, and this. | Promising method target once CSV_INLINE is explicitly supported; header dependencies already include the container types. |
| Same file, lines 58-65, CSVFormat::try_delimiter overloads | Resolve a call to another member overload and construction of vector<char>. | Demonstrates why member lookup, template dependencies, and implicit operations are higher-value additions than exotic declarator syntax. |
| Same file, exception-gated convenience methods | Identify exactly which configurations contain a candidate and its dependencies. | Under the issues' initial conditional-region policy these remain skipped; preprocessing alone should not silently turn a portable source into one configured variant. |

The neighboring CSV parser was inspected read-only as a representative consumer, not modified. Its guidance already requires CSV_INLINE on many out-of-class definitions. Under a blanket rejection of all macro-shaped signatures, those definitions would remain skipped even with perfect type analysis. A carefully specified empty/inline annotation macro is consequently a high-value follow-up; arbitrary signature-generating macros are not.

Likewise, rejecting every traditional include guard excludes otherwise ordinary headers. Recommend an explicit amendment allowing canonical whole-file guards that remain intact around the replacement. Continue skipping feature conditionals. That is a contract change to the original issues, not an automatic consequence of adding a preprocessor.

The greatest expected return is ordinary non-template functions and methods using already-visible types and APIs. Pimpl methods, platform-specific implementations, internal-linkage state, source-only dependencies, and generic templates remain less promising targets. Methods can be particularly useful because other members are available through class scope; a simplistic textual declaration-order rule would reject valid cases unnecessarily.

## Recommended integration

Use LibTooling and a syntax/semantic frontend action, with AST traversal or matchers for discovery. Clang describes LibTooling as suitable for standalone refactoring and full AST access. Its C++ interface is version-sensitive; pin and test a supported LLVM/Clang release. Libclang provides a more stable C interface, but exposing the detailed implicit operations and preprocessing state needed here favors LibTooling. [Clang interface guide](https://clang.llvm.org/docs/Tooling.html).

This is a subset of compiler functionality, not necessarily a tiny binary. The optional backend brings Clang frontend libraries and LLVM support dependencies. Measure package size, startup time, parse memory, and configuration friction during the prototype. Preserve a build without Clang through a CMake option such as `SINGLE_HEADER_WITH_CLANG`; requesting stitching in that build should give an actionable unsupported-feature error, never silently fall back to heuristic rewriting.

Require compile_commands.json or explicit, complete frontend arguments for each analyzed source. Preserve working directories, target/ABI, language standard, include paths, defines/undefines, forced includes, and semantic flags. Do not combine incompatible per-source flags into a union. Missing or unsupported compilation contexts are explicit analysis failures/skips. Multiple configurations for one file must be selected or checked deliberately. [Compilation database specification](https://clang.llvm.org/docs/JSONCompilationDatabase.html).

The local build uses Visual Studio. CMake's compile-command export is implemented for Makefile and Ninja generators, so this setup needs an analysis build using a supported generator or explicitly supplied frontend arguments. A database from GCC or MSVC still needs compatible Clang arguments and system-header setup; a JSON file alone does not guarantee compatibility. [CMake compile-command export](https://cmake.org/cmake/help/latest/variable/CMAKE_EXPORT_COMPILE_COMMANDS.html).

Use the frontend's preprocessor callbacks to collect includes, macro activity, and skipped ranges; supplement them with physical source tracking for inactive text and directives. Retain original source instead of emitting a preprocessed translation unit, which would flatten configuration choices. SourceManager provides the source and macro-location machinery; precise replacements should operate only on validated file ranges. [PPCallbacks](https://clang.llvm.org/doxygen/classclang_1_1PPCallbacks.html), [SourceManager](https://clang.llvm.org/doxygen/classclang_1_1SourceManager.html).

No optimizer or linker is needed on the generator's execution path. Frontend constant evaluation and template instantiation needed for semantic analysis do remain necessary. Generic data-flow analysis, alias analysis, symbolic execution, and whole-program optimization are not first-release requirements: bodies are moved intact rather than algorithmically rewritten.

## Transformation pipeline

1. **Build the original semantic baseline.** Parse source translation units with their actual contexts. Collect declaration/definition relationships and original semantic records. Restrict edits to scanned sources and emitted project headers. A dependency can be analyzed without becoming an editable input. Missing headers or compiler errors do not authorize best-effort semantic claims.
2. **Construct the emission model.** Preserve original file ranges through include stripping, header ordering, macro rewriting, source collation, and implementation-marker insertion. The generator's current pipeline in `src/amalgamator.cpp:315` must expose this provenance; validating isolated rewritten files would miss effects of final collation.
3. **Classify candidates.** Start with ordinary non-template external free functions and ordinary/static methods in non-template classes. Support aliases, class/container types, references, normal control flow, locals, and resolved calls when the semantic validator covers their operations. Skip conditional candidates, arbitrary signature macros, unsupported attributes, modules, special members/operators, virtual definitions, and unresolved/dependent constructs initially.
4. **Plan paired edits.** Retain the header declaration's defaults, qualifiers, attributes, access location, and documentation. Supply definition parameter names using AST-backed token ranges and recheck defaults and bindings, rather than requiring textual name equality or blindly renaming body text. Handle repeated declarations through redeclaration identity; choose one insertion site and retain the others. Add inline for external free definitions, and use implicit inline for in-class definitions in the supported C++17 context. Remove the corresponding source definition only in the same successful transaction.
5. **Reanalyze the actual tentative artifact.** Use virtual/in-memory files to parse the generated output and supported consumer harnesses with the intended output configuration. Compare the semantic records and expanded tokens with the original baseline, accounting for the explicit header-distribution changes. Check mapped residual code as well as stitched bodies. Do not treat merely compiling as success. If the legacy artifact itself fails, report that baseline limitation separately from a relocation rejection.
6. **Validate the accepted batch.** Candidates can interact through declaration visibility and macro/order changes. Stage discovery in batches, then validate the complete accepted edit set. Remove failed candidates and revalidate the resulting set; cap retries and fall back for an inseparable group. The final validation must correspond to exactly the output that is written.
7. **Emit and report.** Keep residual sources in the existing append/splice path. Report accepted/skipped counts, source locations, reason codes, analyzed configurations, and retained implementation requirements. The `off` path remains byte-for-byte compatible. Never remove helpers/includes or synthesize declarations as an incidental repair.

Initial dependency handling is observational: if an already-visible header API retains the same binding, accept it. If moving the body needs a new include, forward declaration, copied helper, or changed qualification, skip with that reason. Automatically repairing these cases introduces new ordering, linkage, overload, and macro effects and deserves a separate stage.

Direct recursion is not intrinsically unsafe. Mutual recursion is not intrinsically unsafe either if the necessary declarations remain visible. Accept or reject from the resolved relationships rather than applying a blanket call prohibition. For virtual methods, moving a definition inline may also affect ABI-specific emission such as key-function/vtable placement; defer that feature even if its body bindings match.

Inline functions still have ODR obligations. Reject references to source-local mutable state or internal-linkage functions for the initial policy, even when a single synthetic consumer compiles. Some language-permitted internal constants could be supported later with a dedicated rule. Preserve and test function-local static identity across consumers when supporting that construct. [C++ one-definition rule](https://eel.is/c++draft/basic.def.odr).

The guarantee is bounded by supported configurations and assumptions about consumers. Parsing one active branch cannot prove behavior under every possible macro setting. A portable-output claim needs an explicit configuration matrix and preservation of untouched branches; no backend should label a single configured analysis universally safe.

## Delivery stages and acceptance gates

| Stage | Deliverable | Evidence required to proceed |
| --- | --- | --- |
| 1 | Optional frontend adapter and analysis-only report. No output edits. | Correct function/method identity, macro provenance, and dependencies on repository fixtures and a representative consumer; recorded parse time/memory/package cost. |
| 2 | Transactional source edits and destination semantic comparison for a declared AST subset. | Accept a std::string wrapper, normal control flow, resolved calls, and simple methods; reject a changed overload even when output compiles. |
| 3 | Free-function mode integrated with final amalgamation and diagnostics. | Existing off-mode goldens unchanged; accepted code compiles, links, and runs across compiler tests; batch interactions and residual code validated. |
| 4 | Ordinary class methods, independent free/method/all selection. | Fields, member calls, static methods, cv/ref/noexcept, aliases, and declaration parameter differences covered. Incremental value measured alongside free functions. |
| 5 | Explicit guard and known annotation-macro policies. | Dedicated equivalence tests for retained whole-file guards and configured empty/inline annotation expansions; coverage gain measured on CSV-style code. |
| Later | Optional include/helper repair, broader macros, special members, or dependent-template relocation. | Separate design and tests driven by actual skip counts, not speculative completeness. |

Use `--function-stitching off|free|methods|all`, default off, with JSON `functionStitching`. Explicit CLI values including off override config. Reserve modes until their implementation exists and reject unavailable ones. Add a frontend-context option and optional versioned JSON report; keep diagnostics off generated stdout and prevent report/output/input path collisions. Preserve config precedence in GitHub Action forwarding.

Measure both matching and actual acceptance. Report candidates, matched pairs, accepted pairs by free/method category, removed source bodies/lines, and dominant skip reasons. Also report functions already inline whose only gain is placement, and how many libraries still need an implementation section. Compare gains before/after each feature on the same corpus and configurations. No numerical coverage estimate is justified from the current fixtures alone.

The first useful threshold is not a large percentage: it is successfully handling ordinary wrappers and methods with class types and calls, while reliably rejecting silent semantic changes. If most remaining skips are source-only dependencies, decide whether repair is worth a separate project. If they are benign annotations, implement the narrower macro policy first.

## Thorough test specification

Use unit tests for analysis records and editing, golden tests for output, and compiler integration tests that compile, link, and run actual consumers. Positive tests assert exactly one emitted definition at the expected site and equivalent behavior. Negative tests assert the expected reason and no half-applied declaration/body edit. If every candidate is skipped, generated text must equal the existing path under the same options.

| Area | Required cases |
| --- | --- |
| Baseline compatibility | Existing goldens; omitted/explicit off mode; source globs, multiple roots, entry filtering, marker present/absent/custom, macro rewrites, stdout, config precedence. |
| Compilation contexts | Relative paths, spaces, command-line defines/undefines, forced includes, language/target changes, missing generated headers, missing source command, unsupported flags, multiple commands per source. No guessed context. |
| Lexical/range fidelity | Comments with fake signatures, strings/chars/escapes, prefixed/custom raw strings, multiline declarations, CRLF/LF, no final newline, adjacent functions, comments outside and inside edit ranges. |
| Preprocessing | Macro-generated names/types, lowercase macros, body macros, identifiers becoming macros only at destination, nested/continued directives, inactive branches, location/stateful macros, pragma-sensitive code, macro rewrites. |
| Guard policy | Pragma once; traditional whole-file guard under initial skip policy; explicit later guard allowance; nested feature conditions still skipped; partial/noncanonical guards rejected. |
| Annotation policy | Explicit known empty/inline macro under original and distribution configurations; unexpected replacement tokens; macro redefinition; token-paste/stringification; annotation changing calling convention or visibility. |
| Identity and types | Typedef/using aliases, qualified definitions, reopened/nested namespaces, overloads, pointer/reference/cv forms, class return types, repeated declarations, true duplicate definitions, static/anonymous namespace entities. |
| Parameters/defaults | Different and unnamed declaration parameters, source parameter names shadowing header names, defaults distributed across redeclarations, default expressions with bindings, no duplicated defaults. |
| Lookup preservation | Changed pick overload; using directive/namespace alias available only in source; hidden friend/ADL; changed implicit conversion; same callee with changed default argument; type/variable shadowing. |
| Normal bodies | Loops, branches, locals, early returns, class temporaries, RAII cleanup, std::string/vector operations, casts, direct and mutual recursion with sufficient declarations. Supported semantics accept; unsupported nodes explain a skip. |
| Completeness/dependencies | Source-only standard include, later project helper, pimpl complete only in source, later class definition, dependency already present in header. No automatic include/helper hoisting. |
| Methods | Non-template class/struct, namespaced/nested ownership, fields declared later in class, private members, member overloads, static methods, const/ref/noexcept variants, source-only type aliases, differing parameter names. |
| Deferred constructs | Constructors/destructors/operators as candidates, virtual/key-function cases, class/function templates, dependent expressions, modules, export/import/visibility attributes, unmodeled pragmas. Dependencies on supported library constructors/operators are separate from relocating those definitions. |
| Linkage/state | External inline address identity across TUs; local static state shared across TUs; mutable namespace-static or anonymous-namespace dependencies rejected; non-inline residual implementation retained. |
| Batch behavior | Individually valid edits invalid together, rejection followed by revalidation, overlapping ranges, duplicate scanned paths, several definitions in one source, ambiguous cross-TU ownership, deterministic acceptance order. |
| Actual generated layout | Inclusion/marker inside namespaces or feature conditionals, source-system-includes after insertion, entry-excluded declarations, macro rewrites, untouched code affected by changed visibility. Validate emitted layout rather than idealized source layout. |
| Configurations | Supported feature toggles on/off; exception-enabled/disabled configurations where applicable; consistent accepted result across the declared matrix or an explicit skip. |
| Differential behavior | Build original multi-file library and generated library under matched settings; compare expected return values, errors, state, and addresses. Include examples that compile both ways but select different overloads. |
| Diagnostics | Stable reason codes, original locations, deterministic order, configuration labels, distinguish unsupported syntax from changed semantics and baseline errors, JSON escaping and write failures. |
| Robustness | Malformed/incomplete input, bounded analysis deadlines, source changes between analysis/edit application, large bodies, edit conflicts, bounded fuzzing of ranges/lexer inputs, regression seeds. |
| Build variants | Lightweight build with no Clang; optional backend on supported pinned frontend; explicit unsupported-mode error; missing frontend resource/system headers; release artifact packaging. |

Fix the test harness before scaling it. `tests/test_cli.cpp:60` uses a process-local script counter in a shared directory, which can collide under parallel CTest. Use isolated per-test/per-process directories, capture stdout/stderr/exit status, and set subprocess deadlines. The existing multi-TU smoke test at line 130 only builds OBJECT libraries. Retain it as a compilation check and add an executable with two consumers plus main to test linking and execution.

Call each accepted free function from both consumers and compare its address across them, so accidental static insertion cannot disguise a linkage problem. For supported local statics, assert shared state. Exercise method overloads and member-function pointers. Use a separate fixture with a one-TU implementation guard for mixed fallback, rather than expecting retained non-inline residual sources to link from every consumer.

Run unit/golden/link/runtime suites with GCC, Clang, and MSVC for generated C++17 output, plus Clang ASan/UBSan on the analysis/edit code. The semantic backend is Clang even when a consumer test uses GCC or MSVC; those checks test portability of results, not equality of all compilers' internal semantics. Pin the frontend API version and test compiler-specific code as supported or explicitly rejected. Add optional bounded fuzzing, and measure regression in parse time and memory on the same sample corpus.

No implementation or compiler prototype was executed for this revised analysis. The earlier review ran four cached-binary CLI/golden/config tests successfully; its fresh build stalled and was interrupted. Those results do not validate the proposed frontend. This revision changes the plan only.
