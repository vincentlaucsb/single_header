#pragma once

#include "model.hpp"
#include "../stitching.hpp"

namespace clang {
class SourceLocation;
class SourceRange;
class SourceManager;
class LangOptions;
}

namespace single_header::stitching {
std::string read_bytes(const std::filesystem::path &path);
std::string file_name(clang::SourceLocation loc, const clang::SourceManager &sm);
Span span(clang::SourceRange range, const clang::SourceManager &sm,
          const clang::LangOptions &lang, const std::string *known_file = nullptr);
std::vector<PhysicalToken> raw_tokens(const std::string &text);
std::vector<Directive> directives(const std::vector<PhysicalToken> &tokens,
                                  unsigned size);
FileFacts inspect_source(const std::string &text);
bool conditional_range(const FileFacts &facts, unsigned begin, unsigned end,
                       bool allow_internal = false);
std::vector<std::string> conditional_context(const FileFacts &facts, unsigned offset);
bool has_tokens_between(const FileFacts &facts, unsigned begin, unsigned end);
bool apply_edits(const Environment &env,
                 const std::vector<const Candidate *> &candidates,
                 SourceOverrides &overrides);
} // namespace single_header::stitching
