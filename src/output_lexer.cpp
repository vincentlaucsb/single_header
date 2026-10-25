#include "output_lexer.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace single_header::output_detail {
bool horizontal(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }
bool identifier(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 128;
}

// Lex after line splicing, while retaining the physical bytes for output edits.
// Raw string payloads are handled from the original source below.
LogicalSource::LogicalSource(std::string_view source) {
  text.reserve(source.size());
  physical.reserve(source.size() + 1);
  for (std::size_t i = 0; i < source.size(); ++i) {
    if (source[i] == '\\' && i + 1 < source.size()) {
      if (source[i + 1] == '\n') { ++i; continue; }
      if (source[i + 1] == '\r' && i + 2 < source.size() && source[i + 2] == '\n') { i += 2; continue; }
    }
    physical.push_back(i);
    text.push_back(source[i]);
  }
  physical.push_back(source.size());
}
std::size_t LogicalSource::logical(std::size_t offset) const {
  return static_cast<std::size_t>(std::lower_bound(physical.begin(), physical.end(), offset) - physical.begin());
}

std::size_t quoted_end(std::string_view text, std::size_t start) {
  const auto quote = text[start];
  for (auto i = start + 1; i < text.size(); ++i) {
    if (text[i] == '\\') { if (i + 1 < text.size()) ++i; }
    else if (text[i] == quote) return i + 1;
  }
  return text.size();
}
std::size_t number_end(std::string_view text, std::size_t start) {
  auto i = start + 1;
  while (i < text.size()) {
    if (identifier(text[i]) || text[i] == '.') ++i;
    else if (text[i] == '\'' && i + 1 < text.size() && identifier(text[i + 1])) i += 2;
    else if ((text[i] == '+' || text[i] == '-') && (text[i - 1] == 'e' || text[i - 1] == 'E' || text[i - 1] == 'p' || text[i - 1] == 'P')) ++i;
    else break;
  }
  return i;
}

// Returns a logical end, but locates the terminator in unmodified raw bytes.
std::size_t raw_end(const LogicalSource &view, std::string_view source, std::size_t start) {
  const auto &text = view.text;
  std::size_t quote = std::string::npos;
  for (const auto prefix : {"R\"", "u8R\"", "uR\"", "UR\"", "LR\""})
    if (text.compare(start, std::char_traits<char>::length(prefix), prefix) == 0) {
      quote = start + std::char_traits<char>::length(prefix) - 1;
      break;
    }
  if (quote == std::string::npos) return start;
  auto open = quote + 1;
  while (open < text.size() && text[open] != '(') {
    if (open - quote > 16 || std::isspace(static_cast<unsigned char>(text[open])) ||
        text[open] == ')' || text[open] == '\\') return text.size();
    ++open;
  }
  if (open == text.size()) return text.size();
  const auto delimiter = text.substr(quote + 1, open - quote - 1);
  const auto close = source.find(")" + delimiter + '"', view.physical[open] + 1);
  return close == std::string_view::npos ? text.size() : view.logical(close + delimiter.size() + 2);
}

std::size_t skip_directive_space(std::string_view text, std::size_t i) {
  for (;;) {
    while (i < text.size() && horizontal(text[i])) ++i;
    if (text.substr(i, 2) != "/*") return i;
    const auto close = text.find("*/", i + 2);
    if (close == std::string_view::npos) return text.size();
    i = close + 2;
  }
}
std::string trim(std::string_view value) {
  auto begin = value.find_first_not_of(" \t\r\n\f\v");
  if (begin == std::string_view::npos) return {};
  return std::string(value.substr(begin, value.find_last_not_of(" \t\r\n\f\v") - begin + 1));
}

// This is a preprocessing-token scan, after line splicing. It never discovers
// directives inside comments, ordinary strings, raw strings, or other directives.
std::vector<Directive> directives(std::string_view source) {
  const LogicalSource view(source);
  const auto &text = view.text;
  std::vector<Directive> result;
  bool line_start = true;
  std::size_t code_epoch = 0;
  std::size_t line_number = 1, line_cursor = 0;
  for (std::size_t i = 0; i < text.size();) {
    if (text[i] == '\n') { line_start = true; ++i; continue; }
    if (horizontal(text[i])) { ++i; continue; }
    if (text.compare(i, 2, "//") == 0 || text.compare(i, 2, "/*") == 0) {
      const bool block = text[i + 1] == '*';
      const auto close = text.find(block ? "*/" : "\n", i + 2);
      const auto end = close == std::string::npos ? text.size() : close + (block ? 2 : 0);
      if (text.find('\n', i) < end) line_start = true;
      i = end; continue;
    }
    if (line_start && (text[i] == '#' || text.compare(i, 2, "%:") == 0)) {
      const auto word_begin = skip_directive_space(text, i + (text[i] == '#' ? 1 : 2));
      auto word_end = word_begin;
      while (word_end < text.size() && identifier(text[word_end])) ++word_end;
      const auto word = text.substr(word_begin, word_end - word_begin);
      const auto operand = skip_directive_space(text, word_end);
      auto header_end = operand;
      if (word == "include" && operand < text.size() && (text[operand] == '<' || text[operand] == '"')) {
        const auto close = text.find(text[operand] == '<' ? '>' : '"', operand + 1);
        if (close != std::string::npos && text.find('\n', operand) > close) header_end = close + 1;
      }
      std::vector<Range> comments;
      auto end = i + (text[i] == '#' ? 1 : 2);
      for (; end < text.size() && text[end] != '\n';) {
        if (end == operand && header_end > operand) { end = header_end; continue; }
        if (text.compare(end, 2, "/*") == 0 || text.compare(end, 2, "//") == 0) {
          const bool block = text[end + 1] == '*';
          const auto close = text.find(block ? "*/" : "\n", end + 2);
          const auto stop = close == std::string::npos ? text.size() : close + (block ? 2 : 0);
          comments.push_back({end, stop});
          end = stop;
        } else {
          const auto raw = raw_end(view, source, end);
          if (raw != end) end = raw;
          else if (text[end] == '"' || text[end] == '\'') end = quoted_end(text, end);
          else if (std::isdigit(static_cast<unsigned char>(text[end]))) end = number_end(text, end);
          else if (identifier(text[end])) {
            while (end < text.size() && identifier(text[end])) ++end;
          } else ++end;
        }
      }
      // Comments are whitespace for guard parsing, but retained byte-for-byte
      // when removing an include directive at its original position.
      auto argument = text.substr(word_end, end - word_end);
      for (const auto comment : comments) {
        const auto begin = std::max(comment.begin, word_end);
        if (begin < comment.end)
          std::fill(argument.begin() + (begin - word_end), argument.begin() + (comment.end - word_end), ' ');
      }
      std::string header;
      if (header_end > operand && trim(argument.substr(header_end - word_end)).empty())
        header = text.substr(operand, header_end - operand);
      auto line = view.physical[i];
      while (line && source[line - 1] != '\n' && horizontal(source[line - 1])) --line;
      if (line && source[line - 1] != '\n') line = view.physical[i];
      line_number += static_cast<std::size_t>(std::count(source.begin() + line_cursor, source.begin() + view.physical[i], '\n'));
      line_cursor = view.physical[i];
      Directive directive{view.physical[i], view.physical[end] + (end < text.size() && text[end] == '\n'),
                          line, code_epoch, line_number, word, trim(argument), header, {}};
      for (const auto comment : comments)
        directive.comments.push_back({view.physical[comment.begin], view.physical[comment.end]});
      result.push_back(std::move(directive));
      i = end; continue;
    }
    line_start = false;
    ++code_epoch;
    const auto raw = raw_end(view, source, i);
    if (raw != i) i = raw;
    else if (text[i] == '"' || text[i] == '\'') i = quoted_end(text, i);
    else if (std::isdigit(static_cast<unsigned char>(text[i])) ||
             (text[i] == '.' && i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1]))))
      i = number_end(text, i);
    else if (identifier(text[i])) {
      while (i < text.size() && identifier(text[i])) ++i;
    } else ++i;
  }
  return result;
}

} // namespace single_header::output_detail
