#include "cli.hpp"

#include "config.hpp"
#include "helpers.hpp"
#include "stitching.hpp"

#include <filesystem>
#include <map>
#include <stdexcept>

namespace fs = std::filesystem;

namespace single_header {
namespace {

struct CliOptions {
  Options options;
  fs::path config_path;
  bool config_set = false;
  bool include_roots_set = false;
  bool source_patterns_set = false;
  bool macro_rewrites_set = false;
  bool implementation_marker_set = false;
  bool output_set = false;
  bool entry_set = false;
  bool stitching_set = false;
  bool database_set = false;
  bool frontend_args_set = false;
  bool report_set = false;
  bool annotations_set = false;
  bool inline_functions_set = false;
  bool deduplicate_includes_set = false;
  bool merge_namespaces_set = false;
  bool include_guard_set = false;
  bool allow_mixed_includes_set = false;
};

std::vector<std::pair<std::string, std::string>>
merge_macro_rewrites(const std::vector<std::pair<std::string, std::string>> &base,
                     const std::vector<std::pair<std::string, std::string>> &overrides) {
  std::map<std::string, std::string> merged;
  for (const auto &[name, value] : base) {
    merged[name] = value;
  }
  for (const auto &[name, value] : overrides) {
    merged[name] = value;
  }

  std::vector<std::pair<std::string, std::string>> result;
  for (const auto &[name, value] : merged) {
    result.emplace_back(name, value);
  }
  return result;
}

CliOptions parse_cli_args(int argc, char **argv) {
  CliOptions cli;

  auto require_value = [&](int *index, const std::string &option_name) -> std::string {
    if (*index + 1 >= argc) {
      throw std::runtime_error(option_name + " expects a value");
    }
    ++(*index);
    return argv[*index];
  };

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      cli.options.show_help = true;
    } else if (arg == "--version") {
      cli.options.show_version = true;
    } else if (arg == "--config") {
      cli.config_path = require_value(&i, arg);
      cli.config_set = true;
    } else if (arg == "--include-root") {
      cli.options.include_roots.emplace_back(require_value(&i, arg));
      cli.include_roots_set = true;
    } else if (arg == "--output") {
      cli.options.output = require_value(&i, arg);
      cli.output_set = true;
    } else if (arg == "--source") {
      cli.options.source_patterns.push_back(require_value(&i, arg));
      cli.source_patterns_set = true;
    } else if (arg == "--rewrite-macro") {
      cli.options.macro_rewrites.push_back(parse_macro_rewrite(require_value(&i, arg)));
      cli.macro_rewrites_set = true;
    } else if (arg == "--implementation-marker") {
      cli.options.implementation_marker = require_value(&i, arg);
      cli.implementation_marker_set = true;
    } else if (arg == "--entry") {
      cli.options.entry = require_value(&i, arg);
      cli.entry_set = true;
    } else if (arg == "--function-stitching") {
      cli.options.function_stitching = require_value(&i, arg);
      cli.stitching_set = true;
    } else if (arg == "--inline-functions" || arg == "--no-inline-functions") {
      cli.options.inline_functions = arg == "--inline-functions";
      cli.inline_functions_set = true;
    } else if (arg == "--compilation-database") {
      cli.options.compilation_database = require_value(&i, arg);
      cli.database_set = true;
    } else if (arg == "--frontend-arg") {
      cli.options.frontend_args.push_back(require_value(&i, arg));
      cli.frontend_args_set = true;
    } else if (arg == "--stitching-report") {
      cli.options.stitching_report = require_value(&i, arg);
      cli.report_set = true;
    } else if (arg == "--deduplicate-includes" || arg == "--no-deduplicate-includes") {
      cli.options.deduplicate_includes = arg == "--deduplicate-includes";
      cli.deduplicate_includes_set = true;
    } else if (arg == "--merge-namespaces" || arg == "--no-merge-namespaces") {
      cli.options.merge_namespaces = arg == "--merge-namespaces";
      cli.merge_namespaces_set = true;
    } else if (arg == "--include-guard") {
      cli.options.include_guard = require_value(&i, arg);
      cli.include_guard_set = true;
    } else if (arg == "--allow-mixed-include") {
      cli.options.allow_mixed_includes.push_back(require_value(&i, arg));
      cli.allow_mixed_includes_set = true;
    } else if (arg == "--inline-annotation") {
      cli.options.inline_annotations.push_back(require_value(&i, arg));
      cli.annotations_set = true;
    } else {
      throw std::runtime_error("unknown argument: " + arg);
    }
  }

  return cli;
}

} // namespace

std::string usage() {
  return R"(single_header - generate a single-header C++ amalgamation

Usage:
  single_header --include-root <path> --output <path> [options]
  single_header --config <path> [options]
  single_header

Options:
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
  --deduplicate-includes        Hoist unguarded literal includes after the configured
                                outer guard's #define, removing their repeats. Default off.
  --no-deduplicate-includes     Disable include hoisting, overriding JSON configuration.
  --include-guard <macro>        Outer guard name required by --deduplicate-includes.
  --allow-mixed-include <operand> Waive mixed guarded/unguarded audit for one literal
                                operand, e.g. '<thread>'. Still hoists; may be repeated.
  --merge-namespaces            Merge adjacent ordinary named namespaces recursively.
                                Opt-in; review external structural macros before enabling.
  --no-merge-namespaces         Disable namespace merging, overriding JSON configuration.
  --function-stitching <mode>    off (default), free, methods, or all. Requires Clang build.
  --inline-functions            Add inline to ordinary external function definitions.
                                Default off; requires Clang and complete frontend contexts.
  --no-inline-functions         Disable function inlining, overriding JSON configuration.
  --compilation-database <path>  compile_commands.json or its directory; one command per source.
                                With --inline-functions, multiple commands cover source branches.
  --frontend-arg <argument>      Complete Clang frontend arguments instead of a database.
                                Repeat for each argument (including -std and include paths).
  --stitching-report <path>      Optional versioned JSON analysis report (never stdout).
  --inline-annotation <name>     Allow a named object macro expanding to empty or inline.
                                May be repeated; other signature macros are skipped.
  --help                         Show this help.
  --version                      Show the generator version.
)";
}

Options parse_args(int argc, char **argv) {
  const auto cli = parse_cli_args(argc, argv);
  if (cli.options.show_help || cli.options.show_version) {
    return cli.options;
  }

  Options options;
  fs::path config_path;
  if (cli.config_set) {
    config_path = cli.config_path;
  } else {
    const fs::path conventional_config = "single_header.json";
    if (fs::exists(conventional_config)) {
      config_path = conventional_config;
    }
  }

  if (!config_path.empty()) {
    options = read_config(config_path);
  }

  if (cli.include_roots_set) {
    options.include_roots = cli.options.include_roots;
  }
  if (cli.source_patterns_set) {
    options.source_patterns = cli.options.source_patterns;
  }
  if (cli.macro_rewrites_set) {
    options.macro_rewrites =
      merge_macro_rewrites(options.macro_rewrites, cli.options.macro_rewrites);
  }
  if (cli.implementation_marker_set) {
    options.implementation_marker = cli.options.implementation_marker;
  }
  if (cli.output_set) {
    options.output = cli.options.output;
  }
  if (cli.entry_set) {
    options.entry = cli.options.entry;
  }
  if (cli.stitching_set) options.function_stitching = cli.options.function_stitching;
  if (cli.database_set) options.compilation_database = cli.options.compilation_database;
  if (cli.frontend_args_set) options.frontend_args = cli.options.frontend_args;
  if (cli.report_set) options.stitching_report = cli.options.stitching_report;
  if (cli.annotations_set) options.inline_annotations = cli.options.inline_annotations;
  if (cli.inline_functions_set) options.inline_functions = cli.options.inline_functions;
  if (cli.deduplicate_includes_set) options.deduplicate_includes = cli.options.deduplicate_includes;
  if (cli.merge_namespaces_set) options.merge_namespaces = cli.options.merge_namespaces;
  if (cli.include_guard_set) options.include_guard = cli.options.include_guard;
  if (cli.allow_mixed_includes_set) options.allow_mixed_includes = cli.options.allow_mixed_includes;

  if (options.include_roots.empty()) {
    throw std::runtime_error("at least one --include-root is required");
  }
  if (options.output.empty()) {
    throw std::runtime_error("--output is required");
  }

  validate_stitching_options(options);
  return options;
}

} // namespace single_header
