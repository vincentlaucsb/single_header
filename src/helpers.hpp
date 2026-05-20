#pragma once

#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace single_header {

std::filesystem::path normalize_path(const std::filesystem::path &path);
bool has_extension(const std::filesystem::path &path, const std::set<std::string> &extensions);
std::vector<std::string> read_lines(const std::filesystem::path &path);
void write_text(const std::filesystem::path &path, const std::string &text);
bool contains_wildcard(const std::string &value);
std::vector<std::filesystem::path> expand_source_pattern(const std::string &pattern_text);
std::string replace_all(std::string text,
                        const std::string &needle,
                        const std::string &replacement);
bool parse_system_include(const std::string &line, std::string *included);
bool parse_local_include(const std::string &line, std::string *included);
std::pair<std::string, std::string> parse_macro_rewrite(const std::string &text);

} // namespace single_header
