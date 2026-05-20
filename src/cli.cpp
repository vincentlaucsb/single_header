#include "cli.hpp"

#include "config.hpp"
#include "helpers.hpp"

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
  --help                         Show this help.
)";
}

Options parse_args(int argc, char **argv) {
  const auto cli = parse_cli_args(argc, argv);
  if (cli.options.show_help) {
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

  if (options.include_roots.empty()) {
    throw std::runtime_error("at least one --include-root is required");
  }
  if (options.output.empty()) {
    throw std::runtime_error("--output is required");
  }

  return options;
}

} // namespace single_header
