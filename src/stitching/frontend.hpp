#pragma once

#include "model.hpp"

namespace clang::tooling { struct CompileCommand; }

namespace single_header::stitching {
ParseResult parse(const clang::tooling::CompileCommand &command,
                  const Environment &env, const std::string *artifact);
} // namespace single_header::stitching
