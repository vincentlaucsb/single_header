#include "namespace_merging.hpp"
#include "output_lexer.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <vector>

namespace single_header {
namespace {
using namespace output_detail;
constexpr auto absent = static_cast<std::size_t>(-1);
enum class Kind { word, literal, punctuation, directive };
struct Token {
  std::string_view value;
  Range bytes;
  Kind kind;
  bool is(std::string_view expected) const { return value == expected; }
  bool opens() const { return is("{") || is("<%"); }
  bool closes() const { return is("}") || is("%>"); }
};

// The common scanner supplies directive spans, line-splicing, and literal rules.
// Comments remain in the source but do not interrupt adjacency.
std::vector<Token> tokens(const LogicalSource &view, std::string_view source,
                          const std::vector<Directive> &items) {
  const auto &text = view.text;
  std::vector<Token> result;
  std::size_t directive = 0;
  for (std::size_t i = 0; i < text.size();) {
    if (std::isspace(static_cast<unsigned char>(text[i]))) { ++i; continue; }
    if (text.compare(i, 2, "//") == 0 || text.compare(i, 2, "/*") == 0) {
      const bool block = text[i + 1] == '*';
      const auto close = text.find(block ? "*/" : "\n", i + 2);
      i = close == std::string::npos ? text.size() : close + (block ? 2 : 0);
      continue;
    }
    while (directive < items.size() && items[directive].end <= view.physical[i]) ++directive;
    auto end = i + 1;
    auto kind = Kind::punctuation;
    if (directive < items.size() && items[directive].begin == view.physical[i]) {
      end = view.logical(items[directive].end);
      kind = Kind::directive;
    } else {
      const auto raw = raw_end(view, source, i);
      auto quote = i;
      for (const auto prefix : {"u8", "u", "U", "L"}) {
        const auto length = std::char_traits<char>::length(prefix);
        if (text.compare(i, length, prefix) == 0 && i + length < text.size() &&
            (text[i + length] == '"' || text[i + length] == '\'')) {
          quote = i + length; break;
        }
      }
      if (raw != i) { end = raw; kind = Kind::literal; }
      else if (text[quote] == '"' || text[quote] == '\'') {
        end = quoted_end(text, quote); kind = Kind::literal;
      } else if (std::isdigit(static_cast<unsigned char>(text[i])) ||
                 (text[i] == '.' && i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1])))) {
        end = number_end(text, i); kind = Kind::literal;
      } else if (identifier(text[i])) {
        while (end < text.size() && identifier(text[end])) ++end;
        kind = Kind::word;
      } else if (text.compare(i, 2, "<%") == 0 || text.compare(i, 2, "%>") == 0) ++end;
    }
    // A user-defined literal suffix is part of the token, not a macro name.
    if (kind == Kind::literal)
      while (end < text.size() && identifier(text[end])) ++end;
    result.push_back({std::string_view(text).substr(i, end - i), {view.physical[i], view.physical[end]}, kind});
    i = end;
  }
  return result;
}

bool keyword(std::string_view word) {
  static const std::set<std::string_view> words = {
    "alignas", "alignof", "asm", "auto", "bool", "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t",
    "class", "const", "consteval", "constexpr", "constinit", "const_cast", "continue", "decltype", "default", "delete",
    "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float", "for", "friend",
    "goto", "if", "inline", "int", "long", "mutable", "new", "noexcept", "nullptr", "operator", "private", "protected",
    "public", "register", "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static", "static_assert",
    "static_cast", "struct", "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid",
    "typename", "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "and", "and_eq", "bitand",
    "bitor", "compl", "not", "not_eq", "or", "or_eq", "xor", "xor_eq"};
  return words.count(word) != 0;
}
bool macro_style(std::string_view word) {
  bool upper = false;
  for (const unsigned char c : word) {
    if (std::islower(c)) return false;
    upper = upper || std::isupper(c);
  }
  return upper;
}

// Every definition participates, including conditional definitions and later
// redefinitions. Function macros, token pasting, braces, namespace tokens, and
// aliases whose full expansion is not known harmless are treated as barriers.
std::set<std::string> risky_macros(const std::vector<Directive> &items,
                                   std::set<std::string> &defined) {
  struct Definition { bool risky = false; std::set<std::string> references; };
  std::map<std::string, Definition> definitions;
  for (const auto &item : items) {
    if (item.word != "define") continue;
    auto end = std::size_t{0};
    while (end < item.argument.size() && identifier(item.argument[end])) ++end;
    if (!end) continue;
    const auto name = item.argument.substr(0, end);
    defined.insert(name);
    auto &definition = definitions[name];
    if (end < item.argument.size() && item.argument[end] == '(') definition.risky = true;
    const auto replacement = std::string_view(item.argument).substr(end);
    const LogicalSource view(replacement);
    for (const auto &token : tokens(view, replacement, {})) {
      if (token.opens() || token.closes() || token.is("namespace") || token.is("#") || token.is("%"))
        definition.risky = true;
      if (token.kind == Kind::word) definition.references.emplace(token.value);
    }
  }
  // Solve harmlessness rather than danger: cycles and unknown external aliases
  // never acquire a proof that they cannot inject structural tokens.
  std::set<std::string> harmless;
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &[name, definition] : definitions) {
      if (definition.risky || harmless.count(name)) continue;
      if (std::all_of(definition.references.begin(), definition.references.end(),
                      [&](const auto &reference) {
                        return (keyword(reference) && !defined.count(reference)) || harmless.count(reference) != 0;
                      })) {
        harmless.insert(name); changed = true;
      }
    }
  }
  std::set<std::string> result;
  for (const auto &name : defined) if (!harmless.count(name)) result.insert(name);
  return result;
}

struct Namespace {
  std::size_t first, open, close = absent, parent;
  std::string_view name;
  bool safe = true;
};
struct Frame { std::size_t node = absent; bool declaration_start = true; };

std::string merge_pass(std::string_view source) {
  const LogicalSource view(source);
  const auto directives_found = directives(source);
  const auto code = tokens(view, source, directives_found);
  std::set<std::string> defined;
  const auto risky = risky_macros(directives_found, defined);
  // Removing a namespace keyword would also remove its macro expansion.
  if (defined.count("namespace")) return std::string(source);
  std::vector<Namespace> nodes;
  std::vector<std::size_t> closing_node(code.size(), absent);
  std::vector<Frame> stack(1);
  auto invalidate = [&] {
    for (const auto &frame : stack) if (frame.node != absent) nodes[frame.node].safe = false;
  };
  for (std::size_t i = 0; i < code.size(); ++i) {
    const auto &token = code[i];
    if (token.kind == Kind::directive) { invalidate(); stack.back().declaration_start = true; continue; }
    const bool namespace_scope = stack.size() == 1 || stack.back().node != absent;
    if (namespace_scope && stack.back().declaration_start && token.is("namespace") && i + 2 < code.size() &&
        code[i + 1].kind == Kind::word && code[i + 2].is("{")) {
      const auto name = code[i + 1].value;
      const bool safe = !defined.count(std::string(name)) && !macro_style(name);
      if (!safe) invalidate();
      nodes.push_back({i, i + 2, absent, stack.back().node, name, safe});
      stack.back().declaration_start = false;
      stack.push_back({nodes.size() - 1, true});
      i += 2;
      continue;
    }
    if (token.kind == Kind::word) {
      const auto name = std::string(token.value);
      const bool ordinary_word = !keyword(token.value) && !token.is("namespace");
      const bool external_style = ordinary_word && !defined.count(name) && macro_style(token.value);
      const bool standalone = ordinary_word && namespace_scope && stack.back().declaration_start && i + 1 < code.size() &&
          (code[i + 1].is("(") || code[i + 1].is(";") || code[i + 1].closes() || code[i + 1].is("namespace"));
      if (risky.count(name) || external_style || (standalone && !defined.count(name))) invalidate();
    }
    if (token.opens()) {
      stack.back().declaration_start = false;
      stack.push_back({});
    } else if (token.closes()) {
      if (stack.size() == 1) return std::string(source);
      if (stack.back().node != absent) {
        nodes[stack.back().node].close = i;
        closing_node[i] = stack.back().node;
      }
      stack.pop_back();
      stack.back().declaration_start = true;
    } else if (token.is(";")) stack.back().declaration_start = true;
    else stack.back().declaration_start = false;
  }
  if (stack.size() != 1) return std::string(source);
  // A linked token stream and namespace representatives retain the one scan's
  // facts. Each merge exposes at most one new child boundary to the work queue.
  // No whole-file re-lex/reparse is needed, even for deeply nested chains.
  std::vector<std::size_t> previous(code.size()), next(code.size()), representative(nodes.size());
  std::vector<bool> active(code.size(), true);
  std::vector<std::size_t> opening_node(code.size(), absent), pending;
  for (std::size_t i = 0; i < code.size(); ++i) {
    previous[i] = i ? i - 1 : absent;
    next[i] = i + 1 < code.size() ? i + 1 : absent;
  }
  for (std::size_t n = 0; n < nodes.size(); ++n) {
    representative[n] = n;
    opening_node[nodes[n].first] = n;
    pending.push_back(nodes[n].first);
  }
  auto canonical = [&](std::size_t n) {
    if (n == absent) return n;
    auto root = n;
    while (representative[root] != root) root = representative[root];
    while (representative[n] != n) {
      const auto parent = representative[n];
      representative[n] = root;
      n = parent;
    }
    return root;
  };
  std::vector<Range> edits;
  auto remove = [&](std::size_t token) {
    edits.push_back(code[token].bytes);
    const auto before = previous[token], after = next[token];
    if (before != absent) next[before] = after;
    if (after != absent) previous[after] = before;
    active[token] = false;
  };
  for (std::size_t work = 0; work < pending.size(); ++work) {
    const auto first = pending[work];
    if (first == absent || !active[first] || opening_node[first] == absent) continue;
    const auto right_index = canonical(opening_node[first]);
    auto &right = nodes[right_index];
    const auto before = previous[first];
    if (!right.safe || before == absent || closing_node[before] == absent) continue;
    const auto left_index = canonical(closing_node[before]);
    auto &left = nodes[left_index];
    if (left_index == right_index || !left.safe || left.close != before || left.name != right.name ||
        canonical(left.parent) != canonical(right.parent)) continue;
    // Comments are not tokens; removing only delimiters and the second header
    // retains all their original bytes, including license and namespace labels.
    const auto exposed = next[right.open];
    remove(left.close);
    for (auto t = right.first; t <= right.open; ++t) remove(t);
    representative[right_index] = left_index;
    left.close = right.close;
    pending.push_back(exposed);
  }
  std::sort(edits.begin(), edits.end(), [](Range a, Range b) { return a.begin < b.begin; });
  std::string output;
  output.reserve(source.size());
  std::size_t cursor = 0;
  for (const auto edit : edits) {
    output.append(source.substr(cursor, edit.begin - cursor));
    cursor = edit.end;
  }
  output.append(source.substr(cursor));
  return output;
}
} // namespace

std::string merge_adjacent_namespaces(std::string_view source) {
  return merge_pass(source);
}
} // namespace single_header
