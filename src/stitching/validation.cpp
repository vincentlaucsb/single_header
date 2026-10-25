#include "validation.hpp"

namespace single_header::stitching {
std::string compare(const ParseResult &before, const ParseResult &after,
                    std::string *detail, bool exact) {
  if (after.errors)
    return "destination_parse_error";
  for (const auto &[key, record] : before.bodies) {
    const auto found = after.bodies.find(key);
    if (found == after.bodies.end())
      return "missing_definition:" + key;
    if (!(record == found->second)) {
      if (detail) {
        const auto &a = record.semantics == found->second.semantics
                            ? record.tokens
                            : record.semantics;
        const auto &b = record.semantics == found->second.semantics
                            ? found->second.tokens
                            : found->second.semantics;
        size_t index = 0;
        while (index < a.size() && index < b.size() && a[index] == b[index])
          ++index;
        const auto start = index > 80 ? index - 80 : 0;
        *detail = "first differing record at " + std::to_string(index) +
                  "\noriginal: " + a.substr(start, 320) +
                  "\ngenerated: " + b.substr(start, 320) + '\n';
      }
      return "changed_semantics:" + key;
    }
  }
  for (const auto &[key, record] : before.declarations) {
    const auto found = after.declarations.find(key);
    if (found == after.declarations.end() || record != found->second) {
      if (detail)
        *detail =
            "declaration record " + key + "\noriginal: " + record +
            "\ngenerated: " +
            (found == after.declarations.end() ? "<missing>" : found->second) +
            '\n';
      return "changed_declaration:" + key;
    }
  }
  if (exact) {
    for (const auto &[key, record] : after.bodies) {
      (void)record;
      if (!before.bodies.count(key))
        return "added_definition:" + key;
    }
    for (const auto &[key, record] : after.declarations) {
      (void)record;
      if (!before.declarations.count(key))
        return "added_declaration:" + key;
    }
  }
  return {};
}

} // namespace single_header::stitching
