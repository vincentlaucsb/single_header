#include "clang_sdk_allocator.hpp"
#include "source.hpp"
#include "../helpers.hpp"

#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Lexer.h>
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace clang;

namespace single_header::stitching {
std::string read_bytes(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("cannot read stitching input: " + path.string());
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}
std::string file_name(SourceLocation loc, const SourceManager &sm) {
  if (loc.isInvalid())
    return {};
  const auto name = sm.getFilename(sm.getExpansionLoc(loc));
  return name.empty() ? std::string{}
                      : normalize_path(fs::path(name.str())).generic_string();
}

Span span(SourceRange range, const SourceManager &sm, const LangOptions &lang) {
  if (range.isInvalid() || range.getBegin().isMacroID() ||
      range.getEnd().isMacroID())
    return {};
  const auto begin = sm.getFileLoc(range.getBegin());
  const auto end =
      Lexer::getLocForEndOfToken(sm.getFileLoc(range.getEnd()), 0, sm, lang);
  if (end.isInvalid() || sm.getFileID(begin) != sm.getFileID(end))
    return {};
  return {file_name(begin, sm), sm.getFileOffset(begin), sm.getFileOffset(end)};
}
std::vector<PhysicalToken> raw_tokens(const std::string &text) {
  LangOptions language;
  language.CPlusPlus = true;
  language.CPlusPlus11 = true;
  Lexer lexer(SourceLocation::getFromRawEncoding(1), language, text.data(),
              text.data(), text.data() + text.size());
  std::vector<PhysicalToken> result;
  Token token;
  while (true) {
    lexer.LexFromRawLexer(token);
    if (token.is(tok::eof))
      break;
    if (token.is(tok::comment))
      continue;
    const auto offset = token.getLocation().getRawEncoding() - 1;
    result.push_back({text.substr(offset, token.getLength()), offset,
                      token.isAtStartOfLine()});
  }
  return result;
}
std::vector<Directive> directives(const std::vector<PhysicalToken> &tokens,
                                  unsigned size) {
  std::vector<Directive> result;
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (!tokens[i].start || tokens[i].text != "#")
      continue;
    size_t end = i + 1;
    while (end < tokens.size() && !tokens[end].start)
      ++end;
    Directive directive{{},
                        tokens[i].offset,
                        end < tokens.size() ? tokens[end].offset : size,
                        i,
                        end};
    for (auto j = i + 1; j < end; ++j)
      directive.words.push_back(tokens[j].text);
    result.push_back(std::move(directive));
  }
  return result;
}
bool conditional_range(const std::string &text, unsigned begin, unsigned end) {
  const auto tokens = raw_tokens(text);
  const auto commands = directives(tokens, static_cast<unsigned>(text.size()));
  bool guard =
      commands.size() >= 3 && commands[0].first == 0 &&
      commands[0].words.size() == 2 && commands[0].words[0] == "ifndef" &&
      commands[1].words.size() == 2 && commands[1].words[0] == "define" &&
      commands[0].words[1] == commands[1].words[1] &&
      commands[1].first == commands[0].last &&
      commands.back().words.size() == 1 &&
      commands.back().words[0] == "endif" &&
      commands.back().last == tokens.size();
  int depth = 0;
  for (const auto &command : commands) {
    if (command.words.empty())
      continue;
    const auto &kind = command.words[0];
    if ((kind == "else" || kind == "elif") && depth == 1)
      guard = false;
    if (kind == "if" || kind == "ifdef" || kind == "ifndef")
      ++depth;
    if (kind == "endif")
      --depth;
    if (kind == "endif" && depth == 0 && &command != &commands.back())
      guard = false;
  }
  // Recompute at the candidate start after determining the whole-file guard.
  depth = 0;
  for (const auto &command : commands) {
    if (command.words.empty())
      continue;
    const auto &kind = command.words[0];
    const bool opens = kind == "if" || kind == "ifdef" || kind == "ifndef";
    const bool branch =
        opens || kind == "else" || kind == "elif" || kind == "endif";
    if (branch && command.begin >= begin && command.begin < end)
      return true;
    if (command.begin > begin)
      break;
    if (opens)
      ++depth;
    if (kind == "endif")
      --depth;
  }
  return depth > (guard ? 1 : 0);
}
bool apply_edits(const Environment &env,
                 const std::vector<const Candidate *> &candidates,
                 SourceOverrides &overrides) {
  std::map<std::string, std::vector<Edit>> by_file;
  for (const auto *candidate : candidates) {
    by_file[candidate->declaration.where.file].push_back(
        candidate->declaration);
    by_file[candidate->definition.where.file].push_back(candidate->definition);
  }
  for (auto &[file, edits] : by_file) {
    auto text = env.input.at(file);
    std::sort(edits.begin(), edits.end(), [](const Edit &a, const Edit &b) {
      return a.where.begin > b.where.begin;
    });
    unsigned boundary = static_cast<unsigned>(text.size());
    for (const auto &edit : edits) {
      if (!edit.where.valid() || edit.where.end > boundary)
        return false;
      text.replace(edit.where.begin, edit.where.end - edit.where.begin,
                   edit.replacement);
      boundary = edit.where.begin;
    }
    overrides[file] = std::move(text);
  }
  return true;
}

} // namespace single_header::stitching
