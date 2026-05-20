# single_header

`single_header` is a small C++17 command-line tool for building a single-header
amalgamation from a directory of headers and sources. It scans include roots,
orders local header dependencies, strips local includes and `#pragma once`
directives, and writes one self-contained header.

The repository also includes a composite GitHub Action that downloads a released
`single_header` binary and can run it in downstream workflows.

## Features

- Scans `.h`, `.hpp`, and `.cpp` files from one or more include roots.
- Preserves system includes while inlining known project-local includes.
- Optionally limits output to headers reachable from an entry header.
- Supports extra source files, directories, or glob patterns.
- Rewrites selected `#define NAME` directives with CLI or JSON config values.
- Splices stripped source text into a configurable implementation marker, or
  appends source text when no marker is found.

## Building

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
--help                         Show help.
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
  "entry": "include/mylib/api.hpp"
}
```

CLI flags override config values. Repeated `--rewrite-macro` flags merge with
config rewrites, with CLI values winning for matching names.

## GitHub Action

Use this repository as a composite action from a release tag:

```yaml
- uses: owner/single_header@v0.1.0
  with:
    include-root: include
    output: dist/library.hpp
```

The action downloads the matching release asset for the runner platform and
exposes the installed executable as the `executable` output. Set `run: "false"`
to install the tool without immediately running it.

## Development

The main implementation lives in `src/`. CLI behavior and golden-output tests
live in `tests/`, with fixtures under `tests/fixtures/`.

Build outputs, fetched dependencies, generated test output, and release archives
are intentionally ignored. Keep source, tests, workflow files, and action
metadata tracked; regenerate local build artifacts as needed.
