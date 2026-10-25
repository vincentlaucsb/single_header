#pragma once

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace single_header::stitching {
struct Span {
  std::string file;
  unsigned begin = 0, end = 0;
  bool valid() const { return !file.empty() && begin < end; }
};
struct IdentityIndex {
  std::set<std::string> files;
  std::string artifact;
  std::map<std::string, std::set<std::string>> origins;
  bool frozen = false;
};
struct TokenInfo {
  std::string file, spelling;
  unsigned offset;
  bool macro;
};
struct Expansion {
  Span where;
  std::string name;
  bool annotation;
  bool body_safe = false;
};
struct Record {
  std::string semantics, tokens, unsupported;
  bool internal = false;
  bool operator==(const Record &other) const {
    return semantics == other.semantics && tokens == other.tokens;
  }
};
struct Edit {
  Span where;
  std::string replacement;
};
struct Candidate {
  std::string id, name, reason, file, diagnostics;
  unsigned line = 0;
  bool method = false, already_inline = false, matched = false;
  Edit declaration, definition;
};
struct ParseResult {
  std::map<std::string, Record> bodies;
  std::map<std::string, std::string> declarations;
  std::vector<Candidate> candidates;
  bool errors = false;
  bool artifact_seen = false;
  bool source_state = false;
  std::string diagnostics;
  std::size_t type_cache_hits = 0, type_cache_entries = 0;
  std::vector<unsigned> artifact_error_offsets;
};
struct PhysicalToken {
  std::string text;
  unsigned offset;
  bool start;
};
struct Directive {
  std::vector<std::string> words;
  unsigned begin, end;
  std::size_t first, last;
};
struct FileFacts {
  std::vector<PhysicalToken> tokens;
  std::vector<Directive> directives;
  bool whole_file_guard = false;
};
struct Environment {
  std::set<std::string> headers, sources;
  std::map<std::string, std::string> input;
  std::map<std::string, FileFacts> lexical;
  std::set<std::string> annotations;
  mutable IdentityIndex identities;
  std::string artifact;
};

} // namespace single_header::stitching
