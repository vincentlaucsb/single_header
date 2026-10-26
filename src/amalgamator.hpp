// single_header
// Copyright (c) 2026 Vincent La
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace single_header {

struct FrontendVariant {
  std::vector<std::filesystem::path> sources;
  std::vector<std::string> args;
};

struct Options {
  std::vector<std::filesystem::path> include_roots;
  std::vector<std::string> source_patterns;
  std::vector<std::pair<std::string, std::string>> macro_rewrites;
  std::string implementation_marker = "/** INSERT_CSV_SOURCES **/";
  std::filesystem::path output;
  std::filesystem::path entry;
  std::string function_stitching = "off";
  std::filesystem::path compilation_database;
  std::vector<std::string> frontend_args;
  std::vector<FrontendVariant> frontend_variants;
  std::filesystem::path frontend_directory;
  std::filesystem::path stitching_report;
  std::vector<std::string> inline_annotations;
  bool inline_functions = false;
  bool deduplicate_includes = false;
  bool merge_namespaces = false;
  std::string include_guard;
  std::vector<std::string> allow_mixed_includes;
  std::filesystem::path config_file;
  bool show_help = false;
  bool show_version = false;
};

std::string generate(const Options &options);

} // namespace single_header
