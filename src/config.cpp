#include "config.hpp"

#include "helpers.hpp"

#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace single_header {
namespace {

fs::path config_relative_path(const fs::path &config_dir, const std::string &value) {
  if (value == "-") {
    return fs::path(value);
  }

  fs::path path(value);
  if (path.is_absolute()) {
    return path;
  }
  return config_dir / path;
}

std::string config_relative_pattern(const fs::path &config_dir, const std::string &value) {
  fs::path path(value);
  if (path.is_absolute()) {
    return path.string();
  }
  return (config_dir / path).string();
}

void reject_unknown_config_keys(const json &document, const fs::path &path) {
  static const std::set<std::string> known_keys = {
    "includeRoots", "output", "sources", "rewriteMacros", "implementationMarker", "entry"};

  for (auto it = document.begin(); it != document.end(); ++it) {
    if (!known_keys.count(it.key())) {
      throw std::runtime_error("unknown key in " + path.string() + ": " + it.key());
    }
  }
}

std::vector<std::string> read_json_string_array(const json &document,
                                                const std::string &key,
                                                const fs::path &path) {
  std::vector<std::string> values;
  if (!document.contains(key)) {
    return values;
  }
  if (!document.at(key).is_array()) {
    throw std::runtime_error(key + " in " + path.string() + " must be an array of strings");
  }

  for (const auto &value : document.at(key)) {
    if (!value.is_string()) {
      throw std::runtime_error(key + " in " + path.string() + " must be an array of strings");
    }
    values.push_back(value.get<std::string>());
  }
  return values;
}

std::string read_json_string(const json &document,
                             const std::string &key,
                             const fs::path &path) {
  if (!document.at(key).is_string()) {
    throw std::runtime_error(key + " in " + path.string() + " must be a string");
  }
  return document.at(key).get<std::string>();
}

} // namespace

Options read_config(const fs::path &config_path) {
  std::ifstream input(config_path);
  if (!input) {
    throw std::runtime_error("failed to read config file " + config_path.string());
  }

  json document;
  try {
    input >> document;
  } catch (const json::exception &error) {
    throw std::runtime_error("failed to parse " + config_path.string() + ": " + error.what());
  }

  if (!document.is_object()) {
    throw std::runtime_error("config file must contain a JSON object: " + config_path.string());
  }

  reject_unknown_config_keys(document, config_path);

  Options options;
  const fs::path config_dir = config_path.parent_path().empty()
                                ? fs::current_path()
                                : config_path.parent_path();

  for (const auto &root : read_json_string_array(document, "includeRoots", config_path)) {
    options.include_roots.push_back(config_relative_path(config_dir, root));
  }

  for (const auto &source : read_json_string_array(document, "sources", config_path)) {
    options.source_patterns.push_back(config_relative_pattern(config_dir, source));
  }

  if (document.contains("output")) {
    options.output = config_relative_path(config_dir, read_json_string(document, "output", config_path));
  }

  if (document.contains("implementationMarker")) {
    options.implementation_marker = read_json_string(document, "implementationMarker", config_path);
  }

  if (document.contains("entry")) {
    options.entry = config_relative_path(config_dir, read_json_string(document, "entry", config_path));
  }

  if (document.contains("rewriteMacros")) {
    const auto &rewrites = document.at("rewriteMacros");
    if (rewrites.is_object()) {
      for (auto it = rewrites.begin(); it != rewrites.end(); ++it) {
        if (!it.value().is_string()) {
          throw std::runtime_error("rewriteMacros values in " + config_path.string() +
                                   " must be strings");
        }
        options.macro_rewrites.emplace_back(it.key(), it.value().get<std::string>());
      }
    } else if (rewrites.is_array()) {
      for (const auto &value : rewrites) {
        if (!value.is_string()) {
          throw std::runtime_error("rewriteMacros in " + config_path.string() +
                                   " must be an object or array of NAME=VALUE strings");
        }
        options.macro_rewrites.push_back(parse_macro_rewrite(value.get<std::string>()));
      }
    } else {
      throw std::runtime_error("rewriteMacros in " + config_path.string() +
                               " must be an object or array of NAME=VALUE strings");
    }
  }

  return options;
}

} // namespace single_header
