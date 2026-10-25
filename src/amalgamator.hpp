// single_header 0.1.0
// Copyright (c) 2026 Vincent La
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace single_header {

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
  std::filesystem::path stitching_report;
  std::vector<std::string> inline_annotations;
  std::filesystem::path config_file;
  bool show_help = false;
};

std::string generate(const Options &options);

} // namespace single_header
