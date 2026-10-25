#include "output_cleanup.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <utility>
#include <vector>

namespace single_header {
namespace {
struct Range { std::size_t begin, end; };
bool horizontal(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }
bool identifier(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 128;
}

// Lex after line splicing, while retaining the physical bytes for output edits.
// Raw string payloads are handled from the original source below.
struct LogicalSource {
  std::string text;
  std::vector<std::size_t> physical;
  explicit LogicalSource(std::string_view source) {
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
  std::size_t logical(std::size_t offset) const {
    return static_cast<std::size_t>(std::lower_bound(physical.begin(), physical.end(), offset) - physical.begin());
  }
};

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

struct DocMode { std::string literal; char fence = 0; std::size_t fence_length = 0; bool uncertain = false; };
bool license_word(std::string word) {
  std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return word.find("copyright") == 0 || word == "license" || word == "licensed" ||
         word == "(c)" || word.find("spdx") == 0;
}
void clean_documentation(std::string_view source, std::size_t begin, std::size_t end,
                         bool block, DocMode &mode, std::vector<Range> &edits) {
  const auto text = source.substr(begin, end - begin);
  if (mode.uncertain) return;
  if (text.find("\\\n") != std::string_view::npos || text.find("\\\r\n") != std::string_view::npos) {
    mode.uncertain = true;
    return;
  }
  const auto content_begin = begin + 3;
  const auto content_end = block ? end - 2 : end;
  for (auto line = content_begin; line < content_end;) {
    const auto newline = source.find('\n', line);
    const auto stop = std::min(content_end, newline == std::string_view::npos ? content_end : newline);
    auto first = line;
    while (first < stop && horizontal(source[first])) ++first;
    if (block && line != content_begin && first < stop && source[first] == '*') {
      ++first;
      while (first < stop && horizontal(source[first])) ++first;
    }
    if (first + 2 < stop && (source[first] == '`' || source[first] == '~') &&
        source[first + 1] == source[first] && source[first + 2] == source[first]) {
      auto fence_end = first;
      while (fence_end < stop && source[fence_end] == source[first]) ++fence_end;
      if (mode.literal.empty()) {
        if (!mode.fence) { mode.fence = source[first]; mode.fence_length = fence_end - first; }
        else if (mode.fence == source[first] && fence_end - first >= mode.fence_length &&
                 std::all_of(source.begin() + fence_end, source.begin() + stop, horizontal)) mode.fence = 0;
      }
    } else if (!mode.fence) {
      for (auto i = first; i < stop; ++i) {
        if ((source[i] != '@' && source[i] != '\\') ||
            (i > first && (source[i - 1] == '@' || source[i - 1] == '\\'))) continue;
        auto word_end = i + 1;
        while (word_end < stop && std::isalpha(static_cast<unsigned char>(source[word_end]))) ++word_end;
        const auto word = source.substr(i + 1, word_end - i - 1);
        if (word_end < stop && identifier(source[word_end])) continue;
        if (!mode.literal.empty()) {
          if (word == "end" + mode.literal) mode.literal.clear();
        } else if (word == "code" || word == "verbatim") {
          mode.literal = std::string(word);
        } else if (i == first && word == "file" &&
                   (word_end == stop || !identifier(source[word_end]))) {
          auto argument = word_end;
          while (argument < stop && horizontal(source[argument])) ++argument;
          auto argument_end = argument;
          if (argument < stop && source[argument] != '@' && source[argument] != '\\') {
            if (source[argument] == '"') {
              argument_end = quoted_end(source.substr(0, stop), argument);
              if (argument_end == stop && source[stop - 1] != '"') argument_end = argument;
            } else {
              while (argument_end < stop && !horizontal(source[argument_end])) ++argument_end;
              if (license_word(std::string(source.substr(argument, argument_end - argument)))) argument_end = argument;
            }
          }
          const auto remove_end = argument_end > argument ? argument_end : word_end;
          edits.push_back({i, remove_end});
          i = remove_end - 1;
        }
      }
    }
    line = stop == content_end ? content_end : stop + 1;
  }
}

bool standard_header(const std::string &name) {
  // cassert/assert.h deliberately support reinclusion after NDEBUG changes.
  static const std::set<std::string> headers = {
      "algorithm", "any", "array", "atomic", "barrier", "bit", "bitset",
      "cctype", "cerrno", "cfenv", "cfloat", "charconv", "chrono", "cinttypes",
      "climits", "clocale", "cmath", "codecvt", "compare", "complex", "concepts",
      "condition_variable", "coroutine", "csetjmp", "csignal", "cstdarg", "cstddef",
      "cstdint", "cstdio", "cstdlib", "cstring", "ctime", "cuchar", "cwchar", "cwctype",
      "deque", "exception", "execution", "expected", "filesystem", "flat_map", "flat_set",
      "format", "forward_list", "fstream", "functional", "future", "generator",
      "initializer_list", "iomanip", "ios", "iosfwd", "iostream", "istream", "iterator",
      "latch", "limits", "list", "locale", "map", "mdspan", "memory", "memory_resource",
      "mutex", "new", "numbers", "numeric", "optional", "ostream", "print", "queue",
      "random", "ranges", "ratio", "regex", "scoped_allocator", "semaphore", "set",
      "shared_mutex", "source_location", "span", "spanstream", "sstream", "stack",
      "stacktrace", "stdexcept", "stop_token", "streambuf", "string", "string_view",
      "strstream", "syncstream", "system_error", "thread", "tuple", "type_traits",
      "typeindex", "typeinfo", "unordered_map", "unordered_set", "utility", "valarray",
      "variant", "vector", "version", "ctype.h", "errno.h", "fenv.h", "float.h",
      "inttypes.h", "limits.h", "locale.h", "math.h", "setjmp.h", "signal.h",
      "stdarg.h", "stddef.h", "stdint.h", "stdio.h", "stdlib.h", "string.h",
      "time.h", "uchar.h", "wchar.h", "wctype.h"};
  return headers.count(name) != 0;
}
} // namespace

static std::string clean_pass(std::string_view source, bool metadata, bool blanks) {
  const LogicalSource view(source);
  const auto &text = view.text;
  std::vector<Range> edits, protected_ranges;
  std::set<std::string> included;
  unsigned braces = 0;
  bool line_start = true;
  DocMode line_doc;
  std::size_t previous_doc_end = std::string_view::npos;
  for (std::size_t i = 0; i < text.size();) {
    if (text[i] == '\n') { line_start = true; ++i; continue; }
    if (horizontal(text[i])) { ++i; continue; }
    const auto raw = raw_end(view, source, i);
    if (raw != i) {
      included.clear();
      protected_ranges.push_back({view.physical[i], view.physical[raw]});
      i = raw; line_start = false; continue;
    }
    if (text[i] == '"' || text[i] == '\'') {
      included.clear();
      const auto end = quoted_end(text, i);
      protected_ranges.push_back({view.physical[i], view.physical[end]});
      i = end; line_start = false; continue;
    }
    if (text.compare(i, 2, "//") == 0 || text.compare(i, 2, "/*") == 0) {
      const bool block = text[i + 1] == '*';
      const auto close = text.find(block ? "*/" : "\n", i + 2);
      const auto end = close == std::string::npos ? text.size() : close + (block ? 2 : 0);
      const auto begin_physical = view.physical[i], end_physical = view.physical[end];
      protected_ranges.push_back({begin_physical, end_physical +
          (!block && end_physical < source.size() && source[end_physical] == '\n')});
      const bool documentation = i + 2 < text.size() &&
          (text[i + 2] == '!' || (text[i + 2] == (block ? '*' : '/') &&
            (i + 3 == text.size() || text[i + 3] != text[i + 2])));
      if (metadata && documentation && (!block || close != std::string::npos)) {
        if (block) {
          DocMode mode;
          clean_documentation(source, begin_physical, end_physical, true, mode, edits);
        } else {
          bool contiguous = previous_doc_end != std::string_view::npos;
          unsigned newlines = 0;
          if (contiguous)
            for (auto p = previous_doc_end; p < begin_physical; ++p)
              if (source[p] == '\n') ++newlines;
              else if (!horizontal(source[p])) contiguous = false;
          if (!contiguous || newlines > 1) line_doc = {};
          clean_documentation(source, begin_physical, end_physical, false, line_doc, edits);
          previous_doc_end = end_physical;
        }
      } else previous_doc_end = std::string_view::npos;
      if (text.find('\n', i) < end) line_start = true;
      i = end; continue;
    }
    if ((text[i] == '#' || text.compare(i, 2, "%:") == 0) && line_start) {
      // Preserve the whole logical directive, including multiline comments/raw literals.
      auto end = i + 1;
      for (; end < text.size() && text[end] != '\n';) {
        if (text.compare(end, 2, "/*") == 0) {
          const auto close = text.find("*/", end + 2);
          end = close == std::string::npos ? text.size() : close + 2;
        } else if (text.compare(end, 2, "//") == 0) {
          const auto close = text.find('\n', end + 2);
          end = close == std::string::npos ? text.size() : close;
        } else {
          const auto raw_literal = raw_end(view, source, end);
          if (raw_literal != end) end = raw_literal;
          else if (std::isdigit(static_cast<unsigned char>(text[end]))) end = number_end(text, end);
          else if (text[end] == '"' || text[end] == '\'') end = quoted_end(text, end);
          else if (identifier(text[end])) {
            while (end < text.size() && identifier(text[end])) ++end;
          }
          else ++end;
        }
      }
      const auto begin_physical = view.physical[i], end_physical = view.physical[end];
      protected_ranges.push_back({begin_physical, end_physical +
          (end_physical < source.size() && source[end_physical] == '\n')});
      auto token = i + (text[i] == '#' ? 1 : 2);
      while (token < end && horizontal(text[token])) ++token;
      const auto word_begin = token;
      while (token < end && identifier(text[token])) ++token;
      const auto word = text.substr(word_begin, token - word_begin);
      bool ordinary_include = false;
      if (metadata && word == "include" && !braces &&
          source.substr(begin_physical, end_physical - begin_physical).find('\\') == std::string_view::npos) {
        while (token < end && horizontal(text[token])) ++token;
        if (token < end && text[token] == '<') {
          const auto close = text.find('>', token + 1);
          if (close < end) {
            const auto header = text.substr(token + 1, close - token - 1);
            auto tail = close + 1;
            while (tail < end && horizontal(text[tail])) ++tail;
            // Keep directives with trailing comments intact rather than discard their text.
            if (tail == end && standard_header(header)) {
              ordinary_include = true;
              if (!included.insert(header).second) {
                auto line = begin_physical;
                while (line && source[line - 1] != '\n' && horizontal(source[line - 1])) --line;
                if (line == 0 || source[line - 1] == '\n')
                  edits.push_back({line, end_physical + (end_physical < source.size() && source[end_physical] == '\n')});
                else
                  edits.push_back({begin_physical, end_physical});
              }
            }
          }
        }
      }
      if (!ordinary_include) included.clear();
      i = end; continue;
    }
    line_start = false;
    // Arbitrary code can expand an externally defined or token-pasted pragma.
    // Reuse only uninterrupted include groups; comments/whitespace are harmless.
    included.clear();
    if (text.compare(i, 2, "<%") == 0) { ++braces; i += 2; continue; }
    if (text.compare(i, 2, "%>") == 0) { if (braces) --braces; i += 2; continue; }
    if (text[i] == '{') ++braces;
    else if (text[i] == '}' && braces) --braces;
    if (std::isdigit(static_cast<unsigned char>(text[i])) ||
        (text[i] == '.' && i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1])))) {
      // A pp-number's apostrophes are digit separators, not character literals.
      i = number_end(text, i);
    } else if (identifier(text[i])) {
      while (i < text.size() && identifier(text[i])) ++i;
    } else ++i;
  }
  std::size_t protected_index = 0;
  bool previous_blank = false;
  for (std::size_t line = 0; blanks && line < source.size();) {
    const auto newline = source.find('\n', line);
    const auto end = newline == std::string_view::npos ? source.size() : newline + 1;
    bool blank = true;
    for (auto p = line; p < end; ++p)
      if (source[p] != '\n' && !horizontal(source[p])) { blank = false; break; }
    while (protected_index < protected_ranges.size() && protected_ranges[protected_index].end <= line) ++protected_index;
    const bool protected_line = protected_index < protected_ranges.size() && protected_ranges[protected_index].begin < end;
    if (blank && !protected_line) {
      if (previous_blank) edits.push_back({line, end});
      previous_blank = true;
    } else previous_blank = false;
    line = end;
  }
  std::sort(edits.begin(), edits.end(), [](Range a, Range b) { return a.begin < b.begin; });
  std::string output;
  output.reserve(source.size());
  std::size_t cursor = 0;
  for (const auto edit : edits) {
    if (edit.begin < cursor) continue;
    output.append(source.substr(cursor, edit.begin - cursor));
    cursor = edit.end;
  }
  output.append(source.substr(cursor));
  return output;
}
std::string cleanup_output(std::string_view source) {
  // Deduplication can expose adjacent blank lines; classify the resulting bytes
  // again before collapsing them. Two bounded passes keep cleanup idempotent.
  return clean_pass(clean_pass(source, true, false), false, true);
}
} // namespace single_header
