#include "helpers.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>

namespace fs = std::filesystem;

namespace single_header {
namespace {

std::string generic_absolute(const fs::path &path) {
  return fs::absolute(path).lexically_normal().generic_string();
}

std::string regex_escape(char ch) {
  static const std::string special = R"(\.^$|()[]{}+)";
  if (special.find(ch) != std::string::npos) {
    return std::string("\\") + ch;
  }
  return std::string(1, ch);
}

std::regex wildcard_regex(const std::string &pattern) {
  std::string result = "^";
  for (char ch : fs::path(pattern).generic_string()) {
    if (ch == '*') {
      result += ".*";
    } else if (ch == '?') {
      result += ".";
    } else {
      result += regex_escape(ch);
    }
  }
  result += "$";
  return std::regex(result);
}

fs::path wildcard_search_root(const fs::path &pattern) {
  fs::path root;
  for (const auto &part : pattern) {
    const auto text = part.string();
    if (contains_wildcard(text)) {
      break;
    }
    root /= part;
  }

  if (root.empty()) {
    return fs::current_path();
  }
  if (!fs::is_directory(root)) {
    return root.parent_path().empty() ? fs::current_path() : root.parent_path();
  }
  return root;
}

} // namespace

fs::path normalize_path(const fs::path &path) {
  return fs::path(generic_absolute(path));
}

bool has_extension(const fs::path &path, const std::set<std::string> &extensions) {
  auto ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return extensions.count(ext) != 0;
}

std::vector<std::string> read_lines(const fs::path &path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("failed to read " + path.string());
  }

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(line + '\n');
  }
  return lines;
}

void write_text(const fs::path &path, const std::string &text) {
  if (path == "-") {
    std::cout << text;
    return;
  }

  if (path.has_parent_path()) {
    fs::create_directories(path.parent_path());
  }

  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("failed to write " + path.string());
  }
  output << text;
}

bool contains_wildcard(const std::string &value) {
  return value.find('*') != std::string::npos || value.find('?') != std::string::npos;
}

std::vector<fs::path> expand_source_pattern(const std::string &pattern_text) {
  fs::path pattern(pattern_text);
  std::vector<fs::path> result;

  if (!contains_wildcard(pattern_text)) {
    fs::path path = normalize_path(pattern);
    if (fs::is_directory(path)) {
      for (const auto &entry : fs::recursive_directory_iterator(path)) {
        if (entry.is_regular_file() && has_extension(entry.path(), {".cpp"})) {
          result.push_back(normalize_path(entry.path()));
        }
      }
    } else if (fs::is_regular_file(path)) {
      result.push_back(path);
    } else {
      throw std::runtime_error("source path does not exist: " + pattern_text);
    }
    return result;
  }

  auto matcher = wildcard_regex(generic_absolute(pattern));
  fs::path root = normalize_path(wildcard_search_root(pattern));
  if (!fs::exists(root)) {
    throw std::runtime_error("glob root does not exist: " + root.string());
  }

  for (const auto &entry : fs::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto candidate = generic_absolute(entry.path());
    if (std::regex_match(candidate, matcher)) {
      result.push_back(normalize_path(entry.path()));
    }
  }
  return result;
}

std::string replace_all(std::string text, const std::string &needle, const std::string &replacement) {
  if (needle.empty()) {
    return text;
  }

  std::size_t pos = 0;
  while ((pos = text.find(needle, pos)) != std::string::npos) {
    text.replace(pos, needle.size(), replacement);
    pos += replacement.size();
  }
  return text;
}

bool parse_system_include(const std::string &line, std::string *included) {
  static const std::regex include_regex(R"(^#include <(.*)>)");
  std::smatch match;
  if (!std::regex_search(line, match, include_regex)) {
    return false;
  }
  *included = match[1].str();
  return true;
}

bool parse_local_include(const std::string &line, std::string *included) {
  static const std::regex include_regex("^#include \"(.*)\"");
  std::smatch match;
  if (!std::regex_search(line, match, include_regex)) {
    return false;
  }
  *included = match[1].str();
  return true;
}

std::pair<std::string, std::string> parse_macro_rewrite(const std::string &text) {
  const auto split = text.find('=');
  if (split == std::string::npos || split == 0) {
    throw std::runtime_error("--rewrite-macro expects NAME=VALUE");
  }
  return {text.substr(0, split), text.substr(split + 1)};
}

} // namespace single_header
