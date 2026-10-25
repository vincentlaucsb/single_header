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
          const clang::LangOptions &lang);
std::vector<PhysicalToken> raw_tokens(const std::string &text);
std::vector<Directive> directives(const std::vector<PhysicalToken> &tokens,
                                  unsigned size);
bool conditional_range(const std::string &text, unsigned begin, unsigned end);
bool apply_edits(const Environment &env,
                 const std::vector<const Candidate *> &candidates,
                 SourceOverrides &overrides);
} // namespace single_header::stitching
