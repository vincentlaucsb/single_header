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
  bool show_help = false;
};

std::string generate(const Options &options);

} // namespace single_header
