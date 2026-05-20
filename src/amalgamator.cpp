#include "amalgamator.hpp"

#include "helpers.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace single_header {
namespace {

struct FileSet {
  std::vector<fs::path> headers;
  std::vector<fs::path> sources;
};

std::string trim_define_line(const std::string &line,
                             const std::vector<std::pair<std::string, std::string>> &rewrites) {
  for (const auto &[name, value] : rewrites) {
    const std::regex define_regex("^#define[ \t]+" + name + "([ \t].*)?\n?$");
    if (std::regex_match(line, define_regex)) {
      return "#define " + name + " " + value + "\n";
    }
  }
  return line;
}

fs::path resolve_known_include(const fs::path &including_file,
                               const std::string &included,
                               bool is_local_include,
                               const std::vector<fs::path> &include_roots,
                               const std::unordered_set<std::string> &known_headers) {
  if (is_local_include) {
    const auto relative_path = normalize_path(including_file.parent_path() / fs::path(included));
    if (known_headers.count(relative_path.generic_string())) {
      return relative_path;
    }
  }

  for (const auto &root : include_roots) {
    const auto candidate = normalize_path(root / fs::path(included));
    if (known_headers.count(candidate.generic_string())) {
      return candidate;
    }
  }

  return {};
}

std::vector<fs::path> known_dependencies(const fs::path &file,
                                         const std::vector<fs::path> &include_roots,
                                         const std::unordered_set<std::string> &known_headers) {
  std::vector<fs::path> result;
  for (const auto &line : read_lines(file)) {
    std::string included;
    if (parse_local_include(line, &included)) {
      auto path = resolve_known_include(file, included, true, include_roots, known_headers);
      if (!path.empty()) {
        result.push_back(path);
      }
    } else if (parse_system_include(line, &included)) {
      auto path = resolve_known_include(file, included, false, include_roots, known_headers);
      if (!path.empty()) {
        result.push_back(path);
      }
    }
  }
  return result;
}

FileSet scan_files(const std::vector<fs::path> &include_roots,
                   const std::vector<std::string> &source_patterns) {
  FileSet files;

  for (const auto &root_arg : include_roots) {
    fs::path root = normalize_path(root_arg);
    if (!fs::is_directory(root)) {
      throw std::runtime_error("include root is not a directory: " + root.string());
    }

    for (const auto &entry : fs::recursive_directory_iterator(root)) {
      if (!entry.is_regular_file()) {
        continue;
      }

      fs::path path = normalize_path(entry.path());
      if (has_extension(path, {".h", ".hpp"})) {
        files.headers.push_back(path);
      } else if (has_extension(path, {".cpp"})) {
        files.sources.push_back(path);
      }
    }
  }

  for (const auto &pattern : source_patterns) {
    auto expanded = expand_source_pattern(pattern);
    files.sources.insert(files.sources.end(), expanded.begin(), expanded.end());
  }

  auto sort_unique = [](std::vector<fs::path> &paths) {
    std::sort(paths.begin(), paths.end(), [](const fs::path &a, const fs::path &b) {
      return a.generic_string() < b.generic_string();
    });
    paths.erase(std::unique(paths.begin(), paths.end(), [](const fs::path &a, const fs::path &b) {
                  return a.generic_string() == b.generic_string();
                }),
                paths.end());
  };

  sort_unique(files.headers);
  sort_unique(files.sources);
  return files;
}

std::unordered_set<std::string> known_path_set(const std::vector<fs::path> &paths) {
  std::unordered_set<std::string> result;
  for (const auto &path : paths) {
    result.insert(path.generic_string());
  }
  return result;
}

std::vector<fs::path> reachable_headers(const fs::path &entry,
                                        const std::vector<fs::path> &headers,
                                        const std::vector<fs::path> &include_roots) {
  const auto known = known_path_set(headers);
  const fs::path normalized_entry = normalize_path(entry);
  if (!known.count(normalized_entry.generic_string())) {
    throw std::runtime_error("--entry was not found among scanned headers: " + entry.string());
  }

  std::vector<fs::path> result;
  std::unordered_set<std::string> visited;

  std::function<void(const fs::path &)> visit = [&](const fs::path &path) {
    const auto key = path.generic_string();
    if (visited.count(key)) {
      return;
    }
    visited.insert(key);
    result.push_back(path);
    for (const auto &dep : known_dependencies(path, include_roots, known)) {
      visit(dep);
    }
  };

  visit(normalized_entry);
  std::sort(result.begin(), result.end(), [](const fs::path &a, const fs::path &b) {
    return a.generic_string() < b.generic_string();
  });
  return result;
}

std::vector<fs::path> order_headers(const std::vector<fs::path> &headers,
                                    const std::vector<fs::path> &include_roots) {
  const auto known = known_path_set(headers);
  std::unordered_map<std::string, fs::path> by_key;
  for (const auto &header : headers) {
    by_key[header.generic_string()] = header;
  }

  std::vector<fs::path> ordered;
  std::unordered_map<std::string, int> state;

  std::function<void(const fs::path &)> visit = [&](const fs::path &path) {
    const auto key = path.generic_string();
    if (state[key] == 2) {
      return;
    }
    if (state[key] == 1) {
      throw std::runtime_error("cyclic local include dependency involving " + key);
    }

    state[key] = 1;
    auto deps = known_dependencies(path, include_roots, known);
    std::sort(deps.begin(), deps.end(), [](const fs::path &a, const fs::path &b) {
      return a.generic_string() < b.generic_string();
    });
    for (const auto &dep : deps) {
      const auto dep_key = dep.generic_string();
      if (known.count(dep_key)) {
        visit(by_key.at(dep_key));
      }
    }
    state[key] = 2;
    ordered.push_back(path);
  };

  for (const auto &header : headers) {
    visit(header);
  }
  return ordered;
}

std::string strip_source_file(const fs::path &path,
                              const std::vector<std::pair<std::string, std::string>> &rewrites) {
  std::string output;
  for (auto line : read_lines(path)) {
    std::string included;
    if (parse_system_include(line, &included)) {
      output += trim_define_line(line, rewrites);
      continue;
    }
    if (parse_local_include(line, &included)) {
      continue;
    }
    if (line.find("#pragma once") != std::string::npos) {
      continue;
    }
    output += trim_define_line(line, rewrites);
  }
  return output;
}

std::string process_header_file(const fs::path &path,
                                const std::unordered_set<std::string> &known_headers,
                                const std::unordered_set<std::string> &processed,
                                const std::vector<fs::path> &include_roots,
                                std::unordered_set<std::string> *missing_includes,
                                const std::string &splice_template,
                                const std::vector<std::pair<std::string, std::string>> &rewrites) {
  std::string output;

  for (auto line : read_lines(path)) {
    std::string included;
    if (parse_system_include(line, &included)) {
      const auto include_path =
        resolve_known_include(path, included, false, include_roots, known_headers);
      if (!include_path.empty()) {
        const auto include_key = include_path.generic_string();
        if (!processed.count(include_key)) {
          missing_includes->insert(include_key);
          output += replace_all(splice_template, "{}", include_key);
        }
        continue;
      }
      output += trim_define_line(line, rewrites);
      continue;
    }
    if (parse_local_include(line, &included)) {
      const auto include_path =
        resolve_known_include(path, included, true, include_roots, known_headers);
      if (!include_path.empty()) {
        const auto include_key = include_path.generic_string();
        if (!processed.count(include_key)) {
          missing_includes->insert(include_key);
          output += replace_all(splice_template, "{}", include_key);
        }
      } else {
        output += trim_define_line(line, rewrites);
      }
      continue;
    }
    if (line.find("#pragma once") != std::string::npos) {
      continue;
    }
    output += trim_define_line(line, rewrites);
  }

  return output;
}

std::string collate_headers(std::vector<fs::path> headers,
                            const std::vector<fs::path> &include_roots,
                            const std::vector<std::pair<std::string, std::string>> &rewrites) {
  std::reverse(headers.begin(), headers.end());
  const auto known = known_path_set(headers);
  const std::string splice_template = "__INSERT_HEADER_HERE__({})\n";

  std::string header_concat;
  std::unordered_set<std::string> processed;
  std::unordered_set<std::string> missing_includes;

  for (const auto &path : headers) {
    const auto key = path.generic_string();
    processed.insert(key);

    auto source = process_header_file(path, known, processed, include_roots,
                                      &missing_includes, splice_template, rewrites);
    if (missing_includes.count(key)) {
      const auto splice_phrase = replace_all(splice_template, "{}", key);
      auto first = header_concat.find(splice_phrase);
      if (first != std::string::npos) {
        header_concat.replace(first, splice_phrase.size(), source + "\n");
      }
      header_concat = replace_all(header_concat, splice_phrase, "");
      missing_includes.erase(key);
    } else {
      header_concat += source;
    }
  }

  return header_concat;
}

std::string collate_sources(const std::vector<fs::path> &sources,
                            const std::vector<std::pair<std::string, std::string>> &rewrites) {
  std::string output;
  for (const auto &source : sources) {
    output += strip_source_file(source, rewrites);
    output += '\n';
  }
  return output;
}

} // namespace

std::string generate(const Options &options) {
  auto files = scan_files(options.include_roots, options.source_patterns);
  if (!options.entry.empty()) {
    files.headers = reachable_headers(options.entry, files.headers, options.include_roots);
  }
  auto ordered_headers = order_headers(files.headers, options.include_roots);
  auto header_concat = collate_headers(ordered_headers, options.include_roots,
                                       options.macro_rewrites);
  auto source_concat = collate_sources(files.sources, options.macro_rewrites);

  if (!options.implementation_marker.empty() &&
      header_concat.find(options.implementation_marker) != std::string::npos) {
    header_concat = replace_all(header_concat, options.implementation_marker, source_concat);
  } else if (!source_concat.empty()) {
    if (!header_concat.empty() && header_concat.back() != '\n') {
      header_concat += '\n';
    }
    header_concat += source_concat;
  }

  return "#pragma once\n" + header_concat;
}

} // namespace single_header
