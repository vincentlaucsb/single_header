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
std::string type_name(clang::QualType type, clang::ASTContext &ctx);
std::string declaration_identity(const clang::Decl *decl, clang::ASTContext &ctx,
                                  IdentityIndex &identities);
std::string function_type(clang::FunctionDecl *fn, clang::ASTContext &ctx,
                           IdentityIndex &identities);
Record function_semantics(clang::FunctionDecl *fn, clang::ASTContext &ctx,
                           IdentityIndex &identities);
std::string variable_initializer(clang::VarDecl *variable, clang::ASTContext &ctx,
                                  IdentityIndex &identities);
std::string field_initializer(clang::FieldDecl *field, clang::ASTContext &ctx,
                               IdentityIndex &identities);
std::string default_argument(clang::FunctionDecl *fn, unsigned parameter,
                              clang::ASTContext &ctx, IdentityIndex &identities);
} // namespace single_header::stitching
