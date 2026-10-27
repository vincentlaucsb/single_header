# Changelog

## 1.2.0

- Analyze compiler contexts concurrently in every stitching/inline phase with
  `--jobs N` (JSON `jobs`, Action `jobs`; default: hardware concurrency).
  Generated output, candidate decisions and diagnostics are independent of the
  job count; `--jobs 1` restores serial analysis. Reports add `work.jobs`.
- Give each frontend parse its own physical file system and resolve relative
  source names against that parse's compile-command directory, so concurrent
  contexts never depend on or change the process working directory.
- Spell AST statement classes without Clang's lazily initialized, unsynchronized
  name table, which could return a null name under concurrent parses.

## 1.1.1

- Make optimized generation usable directly from a JSON config, including
  source-specific frontend variants and config-relative compiler arguments.
- Publish a Linux Clang-backend package with its driver, resource headers and
  runtime libraries; the Action can select this package without building LLVM.
- Let explicit CLI compiler-context options replace the configured context.

## 1.1.0

- Add opt-in compiler-backed automatic function inlining, including residual
  definitions and stitching-OFF generation, with C++11 support in inline-only
  mode and source-declaration coverage across multiple compiler configurations.
- Support contained function-like body macros during stitching while preserving
  their invocation spelling and compiler validation in each supplied context.
- Allow the Action to use a locally built executable and forward compiler-backed
  generation options.
- Remove complete Doxygen file-documentation blocks, including descriptions,
  while retaining legal notices, literal examples, and token separation.
- Merge adjacent namespaces based on structural C++ and preprocessing facts.
  Uppercase identifiers, nonstructural macros, pragmas, and internal conditionals
  no longer disqualify entire namespace bodies.
- Sort the hoisted include block alphabetically by header path.

## 1.0.0 (2026-10-25)

- Establish the documented CLI, JSON configuration, GitHub Action inputs/outputs,
  and versioned stitching-report schema as the public compatibility surface.
- Add optional LLVM/Clang 20-backed function and method stitching, with exact
  generated-artifact validation, per-definition diagnostics, and cached batch
  analysis. Supported definitions move into their existing declarations.
- Add optional include hoisting after a configurable outer header guard. Detect
  mixed guarded/unguarded dependencies before transformation; explicit per-header
  waivers suppress only the audit and retain normal hoisting behavior.
- Add opt-in recursive merging of supported adjacent namespace definitions after
  include processing. Preserve declaration order and preprocessing boundaries.
- Automatically remove Doxygen file directives and collapse ordinary repeated
  blank lines while protecting literal/comment content.
- Add `--version`, sourced from the CMake project version. Release builds check out
  the requested tag and verify its agreement with the configured source version.
- Expand compiler, runtime, configuration, lexical, Action, and sanitizer coverage.
- Compare evaluated source-location values when validating relocation, including
  residual bodies and default-expression call contexts.

Generated whitespace, namespace grouping, include layout, byte counts, and
diagnostic wording may evolve. They are not byte-for-byte compatibility promises.
Compiler-backed stitching covers its documented syntax and supplied compilation
contexts; optional textual transformations have their documented preconditions.
Release binaries use the lightweight backend; stitching requires an LLVM-enabled
build. See [README](README.md) and [stitching documentation](docs/function-stitching.md).

## 0.1.0

- Initial lightweight amalgamation CLI and GitHub Action.
