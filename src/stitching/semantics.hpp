#pragma once

#include "model.hpp"

namespace clang {
class ASTContext;
class Decl;
class FieldDecl;
class FunctionDecl;
class QualType;
class VarDecl;
}

namespace single_header::stitching {
// AST pointer keys never escape the lifetime of the owning frontend parse.
struct SemanticCache {
  std::map<const void *, std::string> types;
  std::size_t type_hits = 0;
};
std::string type_name(clang::QualType type, clang::ASTContext &ctx, SemanticCache &cache);
std::string declaration_identity(const clang::Decl *decl, clang::ASTContext &ctx,
                                  IdentityIndex &identities, SemanticCache &cache);
std::string function_type(clang::FunctionDecl *fn, clang::ASTContext &ctx,
                           IdentityIndex &identities, SemanticCache &cache);
Record function_semantics(clang::FunctionDecl *fn, clang::ASTContext &ctx,
                           IdentityIndex &identities, SemanticCache &cache);
std::string variable_initializer(clang::VarDecl *variable, clang::ASTContext &ctx,
                                  IdentityIndex &identities, SemanticCache &cache);
std::string field_initializer(clang::FieldDecl *field, clang::ASTContext &ctx,
                               IdentityIndex &identities, SemanticCache &cache);
std::string default_argument(clang::FunctionDecl *fn, unsigned parameter,
                              clang::ASTContext &ctx, IdentityIndex &identities, SemanticCache &cache);
} // namespace single_header::stitching
