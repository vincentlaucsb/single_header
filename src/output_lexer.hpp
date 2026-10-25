#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace single_header::output_detail {
struct Range { std::size_t begin, end; };
bool horizontal(char c);
bool identifier(char c);

// Preprocessing line splices are removed for recognition; edits use physical bytes.
struct LogicalSource {
  std::string text;
  std::vector<std::size_t> physical;
  explicit LogicalSource(std::string_view source);
  std::size_t logical(std::size_t offset) const;
};
std::size_t quoted_end(std::string_view text, std::size_t start);
std::size_t number_end(std::string_view text, std::size_t start);
std::size_t raw_end(const LogicalSource &view, std::string_view source, std::size_t start);
struct Directive {
  std::size_t begin, end, line, code_epoch, line_number;
  std::string word, argument, header;
  std::vector<Range> comments;
};
std::size_t skip_directive_space(std::string_view text, std::size_t i);
std::string trim(std::string_view value);
std::vector<Directive> directives(std::string_view source);
} // namespace single_header::output_detail
