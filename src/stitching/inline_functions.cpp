#include "clang_sdk_allocator.hpp"
#include "inline_functions.hpp"
#include "source.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Lexer.h>
#include <algorithm>

using namespace clang;

namespace single_header::stitching {
namespace {
void reject_instantiation(SourceLocation location, const std::string &name,
                          ASTContext &ctx, const Environment &env, ParseResult &result) {
  const auto &sm = ctx.getSourceManager();
  const auto file = file_name(location, sm);
  if (env.sources.count(file) && result.inline_problem.empty())
    result.inline_problem = "cannot inline explicit instantiation definition " + name +
        " at " + file + ':' + std::to_string(sm.getExpansionLineNumber(location)) +
        ": explicit instantiation definitions require a single translation-unit owner";
}
}
void collect_inline_class_instantiation(ClassTemplateSpecializationDecl *decl,
                                        ASTContext &ctx, const Environment &env,
                                        ParseResult &result) {
  if (decl->getSpecializationKind() == TSK_ExplicitInstantiationDefinition)
    reject_instantiation(decl->getPointOfInstantiation(), decl->getQualifiedNameAsString(),
                         ctx, env, result);
}
void collect_inline_function(FunctionDecl *fn, ASTContext &ctx,
                             const Environment &env, ParseResult &result) {
  if (env.inline_functions && fn->getTemplateSpecializationKind() == TSK_ExplicitInstantiationDefinition) {
    reject_instantiation(fn->getPointOfInstantiation(), fn->getQualifiedNameAsString(),
                         ctx, env, result);
    return;
  }
  if (!env.inline_functions || fn->isImplicit() ||
      !fn->isThisDeclarationADefinition())
    return;
  const auto &sm = ctx.getSourceManager();
  const auto file = file_name(fn->getLocation(), sm);
  if (!env.input.count(file))
    return;
  const auto physical = Lexer::makeFileCharRange(sm.getExpansionRange(fn->getSourceRange()),
                                                sm, ctx.getLangOpts());
  if (physical.isValid() && physical.getBegin().isFileID() && physical.getEnd().isFileID() &&
      sm.getFileID(physical.getBegin()) == sm.getFileID(physical.getEnd()))
    result.definition_ranges.push_back({file_name(physical.getBegin(), sm),
        sm.getFileOffset(physical.getBegin()), sm.getFileOffset(physical.getEnd())});
  if (fn->isInlined() || fn->isConstexpr() || !fn->isExternallyVisible() ||
      fn->isDependentContext())
    return;
  // Primary templates and their instantiations already have template ODR rules.
  // An explicit specialization is an ordinary definition and needs inline.
  if (fn->getTemplatedKind() != FunctionDecl::TK_NonTemplate &&
      fn->getTemplateSpecializationKind() != TSK_ExplicitSpecialization)
    return;
  auto fail = [&](const std::string &reason) {
    if (result.inline_problem.empty())
      result.inline_problem = "cannot inline " + fn->getQualifiedNameAsString() +
          " at " + file + ':' + std::to_string(sm.getExpansionLineNumber(fn->getLocation())) +
          ": " + reason;
  };
  if (fn->isMain()) {
    fail("main cannot be declared inline");
    return;
  }
  if (fn->isReplaceableGlobalAllocationFunction()) {
    fail("replacement allocation functions cannot be declared inline");
    return;
  }
  auto location = fn->getBeginLoc();
  if (location.isMacroID()) {
    if (isa<CXXConstructorDecl>(fn) || isa<CXXDestructorDecl>(fn) || isa<CXXConversionDecl>(fn))
      location = fn->getQualifierLoc() ? fn->getQualifierLoc().getBeginLoc() :
                                        fn->getNameInfo().getBeginLoc();
    else
      location = fn->getTypeSpecStartLoc();
  }
  if (location.isInvalid() || location.isMacroID() || fn->getLocation().isMacroID()) {
    fail("macro-generated declaration has no supported physical insertion point");
    return;
  }
  if (file_name(location, sm) != file) {
    fail("declaration spans physical files");
    return;
  }
  auto offset = sm.getFileOffset(location);
  const auto &facts = env.lexical.at(file);
  auto token = std::lower_bound(facts.tokens.begin(), facts.tokens.end(), offset,
      [](const PhysicalToken &entry, unsigned value) { return entry.offset < value; });
  // Attributes must remain before decl-specifiers: [[nodiscard]] inline int f().
  // FunctionDecl's start may also include the explicit-specialization prefix.
  if (token != facts.tokens.end() && token->text == "template") {
    ++token;
    if (token == facts.tokens.end() || token->text != "<") {
      fail("unsupported specialization prefix");
      return;
    }
    ++token;
    if (token == facts.tokens.end() || token->text != ">") {
      fail("unsupported specialization prefix");
      return;
    }
    ++token;
  }
  while (token != facts.tokens.end() && token->text == "[" &&
         std::next(token) != facts.tokens.end() && std::next(token)->text == "[") {
    unsigned depth = 0;
    do {
      if (token->text == "[") ++depth;
      if (token->text == "]") --depth;
      ++token;
    } while (token != facts.tokens.end() && depth);
  }
  if (token == facts.tokens.end()) {
    fail("unsupported declaration range");
    return;
  }
  offset = token->offset;
  result.inline_edits.push_back({{file, offset, offset}, "inline "});
}
}
