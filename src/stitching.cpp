#include "stitching.hpp"

#include "helpers.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>

namespace single_header {

bool paths_alias(const std::filesystem::path &left,
                 const std::filesystem::path &right) {
  if (left.empty() || right.empty() || left == "-" || right == "-")
    return false;
  std::error_code error;
  if (std::filesystem::equivalent(left, right, error) && !error)
    return true;
  auto a = std::filesystem::weakly_canonical(std::filesystem::absolute(left))
               .generic_string();
  auto b = std::filesystem::weakly_canonical(std::filesystem::absolute(right))
               .generic_string();
#ifdef _WIN32
  std::transform(a.begin(), a.end(), a.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  std::transform(b.begin(), b.end(), b.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
#endif
  return a == b;
}

void validate_stitching_options(const Options &options) {
  static const std::set<std::string> modes{"off", "free", "methods", "all"};
  if (!modes.count(options.function_stitching)) {
    throw std::runtime_error(
        "--function-stitching expects off, free, methods, or all");
  }
  if (options.function_stitching == "off" && !options.inline_functions) {
    return;
  }
#ifndef SINGLE_HEADER_WITH_CLANG
  throw std::runtime_error(
      std::string(options.inline_functions ? "--inline-functions" : "function stitching") +
      " requires a build configured with "
      "-DSINGLE_HEADER_WITH_CLANG=ON and LLVM/Clang development packages");
#else
  if (options.compilation_database.empty() == options.frontend_args.empty()) {
    throw std::runtime_error(
        std::string(options.inline_functions ? "--inline-functions" : "function stitching") +
        " requires exactly one of --compilation-database "
        "or one or more --frontend-arg arguments (complete Clang context)");
  }
  if (!options.frontend_variants.empty() &&
      (!options.compilation_database.empty() || !options.inline_functions))
    throw std::runtime_error("frontendVariants requires frontendArgs and inlineFunctions");
  for (const auto &name : options.inline_annotations) {
    for (const auto &[macro, value] : options.macro_rewrites) {
      if (macro == name && value != "" && value != "inline")
        throw std::runtime_error(
            "inline annotation rewrite must be empty or inline: " + name);
    }
  }
  const auto database_file =
      std::filesystem::is_directory(options.compilation_database)
          ? options.compilation_database / "compile_commands.json"
          : options.compilation_database;
  if (paths_alias(options.output, options.config_file) ||
      paths_alias(options.output, database_file))
    throw std::runtime_error("stitching output must be distinct from config "
                             "and compilation database");
  if (!options.stitching_report.empty()) {
    const auto report = normalize_path(options.stitching_report);
    if (options.stitching_report == "-" ||
        paths_alias(report, options.output) ||
        paths_alias(report, options.config_file) ||
        paths_alias(report, database_file)) {
      throw std::runtime_error(
          "stitching report must be a file distinct from generated output");
    }
    for (const auto &root : options.include_roots) {
      auto directory = std::filesystem::weakly_canonical(report).parent_path();
      while (!directory.empty()) {
        if (paths_alias(directory, root))
          throw std::runtime_error(
              "stitching report must be outside scanned include roots");
        const auto parent = directory.parent_path();
        if (parent == directory)
          break;
        directory = parent;
      }
    }
  }
#endif
}

#ifndef SINGLE_HEADER_WITH_CLANG
std::string stitch_functions(const Options &,
                             const std::vector<std::filesystem::path> &,
                             const std::vector<std::filesystem::path> &,
                             const RenderArtifact &) {
  throw std::runtime_error("function stitching backend is unavailable");
}
#endif

} // namespace single_header
