# single_header

![single_header wordmark](assets/single-header-wordmark.svg)

`single_header` is a small C++17 command-line tool for building a single-header
amalgamation from a directory of headers and sources. It scans include roots,
orders local header dependencies, strips local includes and `#pragma once`
directives, and writes one self-contained header.

The repository also includes a composite GitHub Action that downloads a released
`single_header` binary and can run it in downstream workflows.

## Features

- Scans `.h`, `.hpp`, and `.cpp` files from one or more include roots.
- Inlines each known project header once, including diamond dependencies and
  repeated quote/angle references to the same header.
- Optionally hoists unguarded literal includes after a configured outer header
  guard's `#define`, then removes repeats of those includes throughout the output.
- Optionally merges supported adjacent named namespaces recursively, preserving
  declaration order and comments.
- Optionally limits output to headers reachable from an entry header.
- Supports extra source files, directories, or glob patterns.
- Rewrites selected `#define NAME` directives with CLI or JSON config values.
- Splices stripped source text into a configurable implementation marker, or
  appends source text when no marker is found.
- Optionally moves supported function/method definitions into their declarations
  with a Clang 20 semantic backend; the default build remains lightweight.
- Automatically collapses repeated ordinary blank lines and removes Doxygen
  `@file`/`\file` documentation blocks, including their descriptions. License-bearing
  blocks retain their text with only the directive/filename removed. Ordinary
  comments, literal examples, and continued directives are preserved.

Cleanup applies to both ordinary generation and stitching. Include deduplication
is **off by default**, so unresolved include directives retain their repetitions.
Enable it with `--deduplicate-includes --include-guard MYLIB_HPP`, or JSON
`"deduplicateIncludes": true` and `"includeGuard": "MYLIB_HPP"`. The guard name has
no default and is never inferred. The configured outer `#ifndef` (or
`#if !defined(...)`) must have one matching opening `#define` and closing `#endif`,
without alternate branches; a missing, invalid, or ambiguous guard is an error.

The optional pass runs on assembled text after stitching edits, before final
blank-line collapse. Before moving anything, it audits literal includes: a header
with both unguarded and conditionally guarded occurrences fails generation. The
configured outer guard is transparent to this check. The error lists every mixed
identity with assembled line numbers and guarding directives; existing output and
stitching-report files remain untouched. Review each dependency and either fix its
include guards upstream or explicitly waive that header with repeatable
`--allow-mixed-include '<thread>'`, JSON `"allowMixedIncludes": ["<thread>"]`, or the
Action's multiline `allow-mixed-include` input. A waiver suppresses only this audit:
the dependency still hoists, and its later guarded duplicates are still removed.
An unrelated waiver never hides another header's conflict. Deduplication off skips
the audit.

The pass gathers all literal `<...>` and `"..."` includes with no
conditional guard except the configured outer guard, in alphabetical header-path order, and
places them immediately after that guard's opening `#define`. Ordinary code,
namespaces, and unrelated macro definitions do not stop the pass. Angle and
quoted names are separate identities. Repeats after the new block are removed,
including guarded repeats; headers appearing only under other conditional guards
stay in place. Conditional includes before the new block remain. Comments and
license text survive at their original locations; continued and digraph include
directives are recognized. Macro-expanded include operands stay unchanged because
the text pass cannot determine their header identity.

Choose this opt-in reordering only for dependencies whose inclusion order, macro
configuration, and C++ scope permit hoisting. This includes assertion and vendor
headers: they receive the same literal-include treatment. The pass makes no general
compiler-equivalence claim. When stitching is enabled, the baseline, trials, final
validation, and report byte counts all use the same fully postprocessed output.
`--no-deduplicate-includes` explicitly overrides an enabled JSON setting.

## Namespace cleanup

Namespace merging is separately opt-in with `--merge-namespaces` (JSON
`"mergeNamespaces": true`, Action `merge-namespaces: "true"`). It runs after include
processing and before final blank-line collapse. `--no-merge-namespaces` overrides
an enabled configuration. Only adjacent ordinary named namespace blocks separated
by whitespace/comments are candidates. Merging outer blocks can expose inner
blocks, which are processed recursively without a fixed nesting limit.

Conditional namespace boundaries and known structural macro expansions prevent
merging. Compact `namespace a::b` syntax and inline/anonymous/attributed namespaces
remain unchanged. Internal conditionals, uppercase identifiers, and nonstructural
macros do not prevent merging. The lightweight pass cannot know
arbitrary macro definitions supplied only by external headers or compiler flags;
use it when those macros do not hide namespace boundaries. It is an optional
structural cleanup, not a general C++ equivalence proof. Compiler-backed stitching
checks the resulting generated artifact under the supplied configurations.
See [namespace cleanup](docs/namespace-merging.md) for supported forms, macro
preconditions, and the cached structural algorithm.

## Building

The optional [`--inline-functions` mode](docs/inline-functions.md) lets the
Clang-enabled generator supply `inline` for ordinary external definitions,
including definitions left unstitched. It works with stitching off and accepts
C++11 contexts in that mode. Conditional source declarations must be covered by
the supplied compiler contexts. Lightweight binaries reject this option clearly.

Requirements:

- CMake 3.20 or newer
- A C++17 compiler

Configure and build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

Run tests:

```sh
ctest --test-dir build --build-config Release --output-on-failure
```

CMake fetches `nlohmann/json` when it is not available from the system package
manager. Tests also fetch Catch2 when needed.

Compiler-backed stitching is an opt-in build requiring LLVM/Clang 20 development
packages. See [function stitching](docs/function-stitching.md) for build commands,
compilation contexts, supported C++, validation guarantees, and limitations.

## CLI Usage

```sh
single_header --include-root include --output dist/library.hpp
```

Common options:

```text
--config <path>                JSON config file. If omitted, ./single_header.json
                               is used when present.
--include-root <path>          Root to scan for .h/.hpp headers and .cpp sources.
                               May be repeated.
--output <path>                Output header path, or '-' for stdout.
--source <glob-or-path>        Extra source file, directory, or wildcard pattern.
                               May be repeated.
--rewrite-macro NAME=VALUE     Rewrite '#define NAME' directives to '#define NAME VALUE'.
                               May be repeated.
--implementation-marker <text> Marker replaced by stripped source text.
--entry <path>                 Optional top-level header; limits output to reachable
                               scanned headers.
--deduplicate-includes        Hoist/deduplicate literal includes; default off.
--no-deduplicate-includes     Disable include hoisting, overriding JSON config.
--include-guard <macro>        Outer guard name required when hoisting is enabled.
--allow-mixed-include <operand> Waive one mixed-guard audit identity; repeatable.
--merge-namespaces            Merge supported adjacent namespaces recursively; default off.
--no-merge-namespaces         Disable namespace merging, overriding JSON config.
--function-stitching <mode>    off (default), free, methods, or all; requires Clang build.
--inline-functions            Add inline to external function definitions; requires Clang.
--no-inline-functions         Disable automatic inlining, overriding JSON config.
--compilation-database <path>  compile_commands.json or its directory.
--frontend-arg <argument>      Complete frontend arguments instead of a database; repeatable.
--jobs <n>                     Compiler contexts analyzed concurrently; default: CPU count.
--stitching-report <path>      Optional versioned JSON analysis report.
--inline-annotation <name>     Opt in an empty/inline annotation macro; repeatable.
--help                         Show help.
--version                      Show the generator version.
```

## JSON Config

When `single_header.json` exists in the working directory, the CLI loads it by
default. You can also pass a config explicitly with `--config`.

```json
{
  "includeRoots": ["include"],
  "output": "dist/library.hpp",
  "sources": ["src/*.cpp"],
  "rewriteMacros": {
    "MYLIB_INLINE": "inline"
  },
  "implementationMarker": "MYLIB_IMPLEMENTATION_MARKER",
  "entry": "include/mylib/api.hpp",
  "deduplicateIncludes": false,
  "inlineFunctions": false,
  "includeGuard": "MYLIB_HPP",
  "allowMixedIncludes": []
}
```

CLI flags override config values. Repeated `--rewrite-macro` flags merge with
config rewrites, with CLI values winning for matching names. Explicit
`--allow-mixed-include` flags replace the JSON waiver list. Include the literal
delimiters in each waiver: `<thread>` and `"thread"` are different identities.
Shell quoting must preserve those delimiters: POSIX shells and PowerShell accept
`--allow-mixed-include '<thread>'` or `--allow-mixed-include '"thread"'`; in Windows
`cmd.exe`, use `--allow-mixed-include "<thread>"` or
`--allow-mixed-include "\"thread\""`.

## GitHub Action

Use this repository as a composite action from a release tag:

```yaml
- uses: owner/single_header@1.1.0
  with:
    include-root: include
    output: dist/library.hpp
```

The action downloads the matching release asset for the runner platform and
accepts `deduplicate-includes: "true"` or `"false"` and `include-guard: MYLIB_HPP`.
Omitting `deduplicate-includes` preserves JSON configuration (or the default off);
an explicit false forwards `--no-deduplicate-includes`. The optional multiline
`allow-mixed-include` input forwards one complete literal operand per line and
replaces configured waivers when supplied. It does not exclude headers from
hoisting. The action also
exposes the installed executable as the `executable` output. Set `run: "false"`
to install the tool without immediately running it.

For compiler-backed generation on Linux x64, select `backend: clang`. That release
archive includes the Clang driver, resource headers and LLVM/Clang runtime
libraries. The host still supplies its platform C++ standard library and system
headers. Other platforms can supply `executable: path/to/single_header` to use a
locally built Clang-enabled binary. The default `backend: lightweight` retains
the original small packages.
The Action forwards `inline-functions`, `function-stitching`,
`compilation-database`, `frontend-arg`, `jobs`, `stitching-report`, and `inline-annotation`
to that executable. See [automatic inlining](docs/inline-functions.md) for the
source-coverage and multi-configuration contract.

All generation settings can live in JSON, so callers can simply run:

```sh
single_header --config single_header.json
```

Use `frontendArgs` for the base compiler context. Optional `frontendVariants`
entries add arguments for selected implementation files; `sources` paths are
relative to the config file, and omitting `sources` applies a variant to every
implementation file. Base contexts are always retained. Relative compiler flags
such as `-Iinclude` resolve from the config directory. For example:

```json
{
  "includeRoots": ["include"],
  "output": "build/library.hpp",
  "inlineFunctions": true,
  "functionStitching": "all",
  "frontendArgs": ["-std=c++20", "-Iinclude"],
  "frontendVariants": [
    {"sources": ["include/platform.cpp"], "args": ["-DOTHER_PLATFORM=1"]}
  ]
}
```

Variants require automatic inlining and explicit frontend arguments. Compilation
databases keep their original working directories. Explicit CLI frontend or
database options replace the corresponding configured context and variants;
CLI frontend paths keep resolving from the invocation directory.

## Development

### Versioning and compatibility

Version 1.0.0 establishes the documented CLI flags, JSON configuration keys,
GitHub Action inputs/outputs, and versioned stitching-report schema as the public
interface. Backward-compatible additions use minor releases; incompatible changes
to that interface require a major release. Consumers of reports should tolerate
new fields and additional diagnostic/reason codes within the same schema version;
incompatible report changes increment
the report's `version` field and follow the public-interface versioning policy.

Generated formatting, include layout, namespace grouping, diagnostic wording,
timings, and byte counts may change between releases. Supported transformations
retain their documented preconditions and limits; exact generated bytes are not
a compatibility guarantee. The lightweight release binaries and optional Clang
backend are identified separately in the documentation. Use `--version` to inspect
the executable version. The CMake project version is the single version source.

See [CHANGELOG.md](CHANGELOG.md) for release notes. The release workflow checks out
the selected release tag and rejects a tag that disagrees with the source version.
It packages the README, changelog, and detailed documentation alongside the binary.

### Repository layout

The main implementation lives in `src/`. CLI behavior and golden-output tests
live in `tests/`, with fixtures under `tests/fixtures/`.

Build outputs, fetched dependencies, generated test output, and release archives
are intentionally ignored. Keep source, tests, workflow files, and action
metadata tracked; regenerate local build artifacts as needed.

## License

MIT License

Copyright (c) 2026 Vincent La

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
