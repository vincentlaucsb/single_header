#include "output_cleanup.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
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

struct Directive {
  std::size_t begin, end, line, code_epoch, line_number;
  std::string word, argument, header;
  std::vector<Range> comments;
};

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

bool opens_guard(const Directive &directive, std::string_view guard) {
  if (directive.word == "ifndef") return directive.argument == guard;
  if (directive.word != "if") return false;
  // Accept the two common forms without interpreting arbitrary expressions.
  auto expression = std::string_view(directive.argument);
  auto i = skip_directive_space(expression, 0);
  if (i == expression.size() || expression[i++] != '!') return false;
  i = skip_directive_space(expression, i);
  if (expression.substr(i, 7) != "defined") return false;
  i += 7;
  if (i < expression.size() && identifier(expression[i])) return false;
  i = skip_directive_space(expression, i);
  const bool parenthesized = i < expression.size() && expression[i] == '(';
  if (parenthesized) i = skip_directive_space(expression, i + 1);
  const auto start = i;
  while (i < expression.size() && identifier(expression[i])) ++i;
  if (expression.substr(start, i - start) != guard) return false;
  i = skip_directive_space(expression, i);
  if (parenthesized) {
    if (i == expression.size() || expression[i++] != ')') return false;
    i = skip_directive_space(expression, i);
  }
  return i == expression.size();
}

std::string hoist_includes(std::string_view source, std::string_view guard,
                           const std::vector<std::string> &allow_mixed_includes) {
  const auto fail = [&](const std::string &reason) -> void {
    throw std::runtime_error("include deduplication: " + reason);
  };
  if (guard.empty() || (!std::isalpha(static_cast<unsigned char>(guard.front())) && guard.front() != '_') ||
      !std::all_of(guard.begin(), guard.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; }))
    fail("--include-guard must name the outer header guard macro");
  const auto items = directives(source);
  const auto missing = items.size();
  std::size_t opening = missing, definition = missing, closing = missing;
  std::vector<std::size_t> stack, branches;
  std::vector<bool> eligible(items.size());
  struct Occurrence { std::size_t line; std::vector<std::size_t> guards; };
  struct IncludeUses { std::vector<Occurrence> unguarded, guarded; };
  std::map<std::string, IncludeUses> uses;
  for (std::size_t i = 0; i < items.size(); ++i) {
    const auto &item = items[i];
    if (opens_guard(item, guard)) {
      if (opening != missing || !stack.empty()) fail("configured guard " + std::string(guard) + " is ambiguous or not outermost");
      opening = i;
    }
    eligible[i] = stack.empty() || (stack.size() == 1 && stack.front() == opening);
    if (!item.header.empty()) {
      Occurrence occurrence{item.line_number, {}};
      for (std::size_t j = 0; j < stack.size(); ++j)
        if (stack[j] != opening) occurrence.guards.push_back(branches[j]);
      auto &use = uses[item.header];
      (eligible[i] ? use.unguarded : use.guarded).push_back(std::move(occurrence));
    }
    if (item.word == "if" || item.word == "ifdef" || item.word == "ifndef") {
      stack.push_back(i);
      branches.push_back(i);
    } else if (item.word == "else" || item.word == "elif" || item.word == "elifdef" || item.word == "elifndef") {
      if (stack.empty()) fail("unmatched #" + item.word);
      if (stack.back() == opening) fail("configured outer guard must not have #else or #elif branches");
      branches.back() = i;
    } else if (item.word == "endif") {
      if (stack.empty()) fail("unmatched #endif");
      if (stack.back() == opening) closing = i;
      stack.pop_back();
      branches.pop_back();
    } else if (item.word == "define" || item.word == "undef") {
      auto end = std::size_t{0};
      while (end < item.argument.size() && identifier(item.argument[end])) ++end;
      if (std::string_view(item.argument).substr(0, end) == guard) {
        if (item.word != "define" || definition != missing || opening == missing ||
            stack.size() != 1 || stack.front() != opening || i != opening + 1 ||
            item.code_epoch != items[opening].code_epoch ||
            (end < item.argument.size() && item.argument[end] == '('))
          fail("configured guard " + std::string(guard) + " needs one opening #define immediately after its guard test");
        definition = i;
      }
    }
  }
  if (!stack.empty()) fail("unterminated conditional directive");
  if (opening == missing || definition == missing || closing == missing)
    fail("configured outer guard " + std::string(guard) + " was not found with a matching #define and #endif");

  std::set<std::string> allowed;
  for (const auto &operand : allow_mixed_includes) {
    if (operand.size() < 3 || (operand.front() != '<' && operand.front() != '"') ||
        operand.back() != (operand.front() == '<' ? '>' : '"') ||
        operand.find_first_of("\r\n") != std::string::npos ||
        operand.find(operand.back(), 1) != operand.size() - 1)
      fail("--allow-mixed-include expects one literal operand including delimiters, e.g. '<thread>'");
    allowed.insert(operand);
  }
  std::ostringstream conflicts;
  for (const auto &[header, use] : uses) {
    if (use.unguarded.empty() || use.guarded.empty() || allowed.count(header)) continue;
    conflicts << "\n  " << header << ": unguarded assembled lines ";
    for (std::size_t i = 0; i < use.unguarded.size(); ++i) {
      if (i) conflicts << ", ";
      conflicts << use.unguarded[i].line;
    }
    conflicts << "; guarded assembled lines ";
    for (std::size_t i = 0; i < use.guarded.size(); ++i) {
      if (i) conflicts << ", ";
      const auto &occurrence = use.guarded[i];
      conflicts << occurrence.line << " [";
      for (std::size_t j = 0; j < occurrence.guards.size(); ++j) {
        if (j) conflicts << "; ";
        const auto &condition = items[occurrence.guards[j]];
        conflicts << '#' << condition.word;
        if (!condition.argument.empty()) conflicts << ' ' << condition.argument;
        conflicts << " (line " << condition.line_number << ')';
      }
      conflicts << ']';
    }
  }
  if (!conflicts.str().empty())
    fail("mixed guarded and unguarded literal includes:" + conflicts.str() +
         "\nUse --allow-mixed-include for each reviewed operand to waive this audit only; those includes still hoist.");

  std::set<std::string> included;
  std::string block;
  const auto newline = source.find("\r\n") == std::string_view::npos ? "\n" : "\r\n";
  for (std::size_t i = 0; i < items.size(); ++i)
    if (eligible[i] && !items[i].header.empty() && included.insert(items[i].header).second)
      block += "#include " + items[i].header + newline;
  if (block.empty()) return std::string(source);
  const auto insertion = items[definition].end;
  if (insertion && source[insertion - 1] != '\n') block.insert(0, newline);
  std::string output;
  std::size_t cursor = 0;
  bool inserted = false;
  for (std::size_t i = 0; i < items.size(); ++i) {
    const auto &item = items[i];
    // Never remove a conditional inclusion preceding the new include block.
    if (item.header.empty() || !included.count(item.header) || (!eligible[i] && item.begin < insertion)) continue;
    if (!inserted && insertion <= item.line) {
      output.append(source.substr(cursor, insertion - cursor));
      output += block;
      cursor = insertion;
      inserted = true;
    }
    output.append(source.substr(cursor, item.line - cursor));
    for (std::size_t c = 0; c < item.comments.size(); ++c) {
      const auto comment = item.comments[c];
      if (c) output += ' ';
      output.append(source.substr(comment.begin, comment.end - comment.begin));
    }
    if (!item.comments.empty() || (item.line > 0 && source[item.line - 1] != '\n'))
      output += newline;
    cursor = item.end;
  }
  if (!inserted) {
    output.append(source.substr(cursor, insertion - cursor));
    output += block;
    cursor = insertion;
  }
  output.append(source.substr(cursor));
  return output;
}
} // namespace

static std::string clean_pass(std::string_view source, bool metadata, bool blanks) {
  const LogicalSource view(source);
  const auto &text = view.text;
  std::vector<Range> edits, protected_ranges;
  bool line_start = true;
  DocMode line_doc;
  std::size_t previous_doc_end = std::string_view::npos;
  for (std::size_t i = 0; i < text.size();) {
    if (text[i] == '\n') { line_start = true; ++i; continue; }
    if (horizontal(text[i])) { ++i; continue; }
    const auto raw = raw_end(view, source, i);
    if (raw != i) {
      protected_ranges.push_back({view.physical[i], view.physical[raw]});
      i = raw; line_start = false; continue;
    }
    if (text[i] == '"' || text[i] == '\'') {
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
      i = end; continue;
    }
    line_start = false;
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
std::string cleanup_output(std::string_view source, bool deduplicate_includes,
                           std::string_view include_guard,
                           const std::vector<std::string> &allow_mixed_includes) {
  const auto assembled = deduplicate_includes ? hoist_includes(source, include_guard, allow_mixed_includes) : std::string(source);
  // Reclassify after metadata edits before the final blank-line collapse.
  return clean_pass(clean_pass(assembled, true, false), false, true);
}
} // namespace single_header
