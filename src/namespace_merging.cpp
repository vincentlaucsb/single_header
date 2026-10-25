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

// Track actual structural tokens and their locally defined macro dependencies.
std::set<std::string> risky_macros(const std::vector<Directive> &items,
                                   std::set<std::string> &defined,
                                   std::set<std::string> &function_macros) {
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
    std::set<std::string> parameters;
    if (end < item.argument.size() && item.argument[end] == '(') {
      function_macros.insert(name);
      const auto close = item.argument.find(')', end);
      if (close == std::string::npos) { definition.risky = true; continue; }
      for (auto p = end + 1; p < close;) {
        if (!identifier(item.argument[p])) { ++p; continue; }
        const auto first = p++;
        while (p < close && identifier(item.argument[p])) ++p;
        parameters.insert(item.argument.substr(first, p - first));
      }
      parameters.insert("__VA_ARGS__");
      end = close + 1;
    }
    const auto replacement = std::string_view(item.argument).substr(end);
    const LogicalSource view(replacement);
    const auto replacement_tokens = tokens(view, replacement, {});
    for (std::size_t t = 0; t < replacement_tokens.size(); ++t) {
      const auto &token = replacement_tokens[t];
      const bool paste = (token.is("#") && t + 1 < replacement_tokens.size() && replacement_tokens[t + 1].is("#")) ||
          (token.is("%") && t + 3 < replacement_tokens.size() && replacement_tokens[t + 1].is(":") &&
           replacement_tokens[t + 2].is("%") && replacement_tokens[t + 3].is(":"));
      if (token.opens() || token.closes() || token.is("namespace") || paste)
        definition.risky = true;
      if (token.kind == Kind::word && !parameters.count(std::string(token.value))) definition.references.emplace(token.value);
    }
  }
  std::set<std::string> result;
  for (const auto &[name, definition] : definitions) if (definition.risky) result.insert(name);
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &[name, definition] : definitions) {
      if (!function_macros.count(name) && std::any_of(definition.references.begin(), definition.references.end(),
          [&](const auto &reference) { return function_macros.count(reference) != 0; })) {
        function_macros.insert(name); changed = true;
      }
      if (result.count(name)) continue;
      if (std::any_of(definition.references.begin(), definition.references.end(),
                      [&](const auto &reference) { return result.count(reference) != 0; })) {
        result.insert(name); changed = true;
      }
    }
  }
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
  std::set<std::string> defined, function_macros;
  const auto risky = risky_macros(directives_found, defined, function_macros);
  // Removing a namespace keyword would also remove its macro expansion.
  if (defined.count("namespace")) return std::string(source);
  std::vector<Namespace> nodes;
  std::vector<std::size_t> closing_node(code.size(), absent);
  std::vector<Frame> stack(1);
  std::vector<std::vector<std::size_t>> conditional_namespaces;
  auto namespaces = [&] {
    std::vector<std::size_t> result;
    for (const auto &frame : stack) if (frame.node != absent) result.push_back(frame.node);
    return result;
  };
  auto invalidate = [&] {
    for (const auto &frame : stack) if (frame.node != absent) nodes[frame.node].safe = false;
  };
  for (std::size_t i = 0; i < code.size(); ++i) {
    const auto &token = code[i];
    if (token.kind == Kind::directive) {
      const auto found = std::lower_bound(directives_found.begin(), directives_found.end(), token.bytes.begin,
          [](const Directive &directive, std::size_t begin) { return directive.begin < begin; });
      if (found == directives_found.end() || found->begin != token.bytes.begin) return std::string(source);
      const auto &word = found->word;
      if (word == "if" || word == "ifdef" || word == "ifndef") conditional_namespaces.push_back(namespaces());
      else if (word == "elif" || word == "elifdef" || word == "elifndef" || word == "else" || word == "endif") {
        // Every branch must leave the namespace scopes it entered. Conditional
        // function/control-flow braces do not change namespace identity.
        if (conditional_namespaces.empty() || conditional_namespaces.back() != namespaces()) return std::string(source);
        if (word == "endif") conditional_namespaces.pop_back();
      } else if (word == "include" || word == "include_next" || word == "import") invalidate();
      stack.back().declaration_start = true;
      continue;
    }
    const bool namespace_scope = stack.size() == 1 || stack.back().node != absent;
    if (namespace_scope && stack.back().declaration_start && token.is("namespace") && i + 2 < code.size() &&
        code[i + 1].kind == Kind::word && code[i + 2].is("{")) {
      const auto name = code[i + 1].value;
      const bool safe = !defined.count(std::string(name));
      if (!safe) invalidate();
      nodes.push_back({i, i + 2, absent, stack.back().node, name, safe});
      stack.back().declaration_start = false;
      stack.push_back({nodes.size() - 1, true});
      i += 2;
      continue;
    }
    if (token.kind == Kind::word) {
      const auto name = std::string(token.value);
      if (risky.count(name)) invalidate();
      if (function_macros.count(name) && i + 1 < code.size() && code[i + 1].is("(")) {
        // Parameters can duplicate or reorder structural argument tokens.
        std::size_t depth = 0;
        for (auto argument = i + 1; argument < code.size(); ++argument) {
          if (code[argument].opens() || code[argument].closes() || code[argument].is("namespace")) invalidate();
          if (code[argument].is("(")) ++depth;
          else if (code[argument].is(")") && --depth == 0) break;
        }
      }
    }
    if (token.opens()) {
      stack.back().declaration_start = false;
      stack.push_back({absent, true});
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
  if (stack.size() != 1 || !conditional_namespaces.empty()) return std::string(source);
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
