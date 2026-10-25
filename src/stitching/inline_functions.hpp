#pragma once

#include "model.hpp"

namespace clang { class FunctionDecl; class ClassTemplateSpecializationDecl; class ASTContext; }

namespace single_header::stitching {
void collect_inline_function(clang::FunctionDecl *function, clang::ASTContext &context,
                             const Environment &environment, ParseResult &result);
void collect_inline_class_instantiation(clang::ClassTemplateSpecializationDecl *declaration,
                                        clang::ASTContext &context,
                                        const Environment &environment, ParseResult &result);
}
