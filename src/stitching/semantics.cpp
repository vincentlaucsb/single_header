#include "clang_sdk_allocator.hpp"
#include "semantics.hpp"
#include "source.hpp"
#include "../helpers.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Attr.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Index/USRGeneration.h>
#include <clang/Lex/Lexer.h>
#include <llvm/ADT/SmallString.h>
#include <sstream>

using namespace clang;

namespace single_header::stitching {
std::string type_name(QualType type, ASTContext &ctx) {
  if (type.isNull())
    return "<null>";
  PrintingPolicy policy(ctx.getLangOpts());
  policy.FullyQualifiedName = true;
  auto result = type.getCanonicalType().getAsString(policy);
  struct Enums : RecursiveASTVisitor<Enums> {
    ASTContext &ctx;
    PrintingPolicy &policy;
    std::string &text;
    std::set<const Type *> visited;
    Enums(ASTContext &context, PrintingPolicy &printing, std::string &value)
        : ctx(context), policy(printing), text(value) {}
    bool TraverseType(QualType type) {
      if (type.isNull() || !visited.insert(type.getTypePtr()).second)
        return true;
      return RecursiveASTVisitor<Enums>::TraverseType(type);
    }
    bool VisitEnumType(EnumType *type) {
      const auto *decl = type->getDecl();
      if (decl->getIdentifier())
        return true;
      llvm::SmallString<256> usr;
      if (!index::generateUSRForDecl(decl, usr)) {
        const auto identity = usr.str().str();
        // Enumerator-based USRs are source independent. Location-based USRs
        // remain unequal and cause a conservative baseline limitation.
        if (identity.compare(0, 3, "c:@") == 0) {
          text = replace_all(std::move(text),
                             QualType(type, 0).getAsString(policy),
                             "enum:" + identity);
        }
      }
      return true;
    }
    bool VisitRecordType(RecordType *type) {
      if (const auto *specialization =
              dyn_cast<ClassTemplateSpecializationDecl>(type->getDecl()))
        for (const auto &argument : specialization->getTemplateArgs().asArray())
          TraverseTemplateArgument(argument);
      return true;
    }
  } enums(ctx, policy, result);
  enums.TraverseType(type.getCanonicalType());
  return result;
}

namespace {
std::string mapped_identity(const Decl *, ASTContext &, IdentityIndex &,
                            const std::string &);

// Named external declarations use Clang USRs. Parameters and locals are
// numbered within corresponding bodies, never compared by AST pointer address.
struct Semantics : RecursiveASTVisitor<Semantics> {
  ASTContext &ctx;
  IdentityIndex *identities;
  std::map<const Decl *, unsigned> locals;
  std::map<const CXXRecordDecl *, std::string> closures;
  std::vector<std::pair<std::string, std::string>> substitutions;
  std::ostringstream out;
  std::string unsupported;
  bool internal_dependency = false;
  unsigned next_local = 0;
  explicit Semantics(ASTContext &context, IdentityIndex *index = nullptr)
      : ctx(context), identities(index) {}

  std::string normalized(std::string value) const {
    for (const auto &[from, to] : substitutions)
      value = replace_all(std::move(value), from, to);
    return value;
  }
  void item(const std::string &value) {
    const auto stable = normalized(value);
    out << stable.size() << ':' << stable << ';';
  }
  std::string template_argument(const TemplateArgument &argument) {
    switch (argument.getKind()) {
    case TemplateArgument::Type:
      return "type:" + normalized(type_name(argument.getAsType(), ctx));
    case TemplateArgument::Integral: {
      llvm::SmallString<40> value;
      argument.getAsIntegral().toString(value, 10);
      return "integer:" + type_name(argument.getIntegralType(), ctx) + ':' +
             value.str().str();
    }
    case TemplateArgument::Declaration:
      return "declaration:" + identity(argument.getAsDecl());
    case TemplateArgument::NullPtr:
      return "null:" + type_name(argument.getNullPtrType(), ctx);
    case TemplateArgument::Template:
      return "template:" +
             identity(argument.getAsTemplate().getAsTemplateDecl());
    case TemplateArgument::Pack: {
      std::string value = "pack:";
      for (const auto &element : argument.pack_elements()) {
        const auto part = template_argument(element);
        value += std::to_string(part.size()) + ':' + part;
      }
      return value;
    }
    default:
      unsupported = "unsupported_template_argument";
      std::string printed;
      llvm::raw_string_ostream stream(printed);
      argument.print(PrintingPolicy(ctx.getLangOpts()), stream, true);
      return "unsupported:" + normalized(printed);
    }
  }
  std::string identity(const Decl *decl) {
    if (!decl) {
      unsupported = "unresolved_identity";
      return "?";
    }
    decl = decl->getCanonicalDecl();
    if (const auto *parameter = dyn_cast<NonTypeTemplateParmDecl>(decl)) {
      return "template-parameter:" + std::to_string(parameter->getDepth()) +
             ':' + std::to_string(parameter->getIndex()) + ':' +
             type_name(parameter->getType(), ctx) +
             (parameter->isParameterPack() ? ":pack" : ":single");
    }
    if (const auto *record = dyn_cast<CXXRecordDecl>(decl)) {
      const auto closure = closures.find(record->getCanonicalDecl());
      if (closure != closures.end())
        return closure->second;
    }
    if (const auto *parent = dyn_cast<CXXRecordDecl>(decl->getDeclContext())) {
      const auto closure = closures.find(parent->getCanonicalDecl());
      if (closure != closures.end()) {
        if (const auto *method = dyn_cast<CXXMethodDecl>(decl)) {
          const auto name = isa<CXXConstructorDecl>(method) ? "constructor"
                            : isa<CXXDestructorDecl>(method)
                                ? "destructor"
                                : method->getNameAsString();
          std::string value = closure->second + ":member:" + name + ':' +
                              normalized(type_name(method->getType(), ctx));
          if (const auto *arguments = method->getTemplateSpecializationArgs()) {
            for (const auto &argument : arguments->asArray()) {
              const auto part = template_argument(argument);
              value += ':' + std::to_string(part.size()) + ':' + part;
            }
          }
          return value;
        }
        if (const auto *field = dyn_cast<FieldDecl>(decl))
          return closure->second +
                 ":field:" + std::to_string(field->getFieldIndex()) + ':' +
                 normalized(type_name(field->getType(), ctx));
      }
    }
    const auto local = locals.find(decl);
    if (local != locals.end())
      return "local:" + std::to_string(local->second);
    if (const auto *named = dyn_cast<NamedDecl>(decl)) {
      if (named->getDeclContext()->isFunctionOrMethod()) {
        const auto number = next_local++;
        locals[decl] = number;
        return "local:" + std::to_string(number);
      }
      if (const auto *value = dyn_cast<ValueDecl>(decl)) {
        if (value->getLinkageInternal() == Linkage::Internal ||
            value->getLinkageInternal() == Linkage::UniqueExternal)
          internal_dependency = true;
      }
    }
    if (const auto *function = dyn_cast<FunctionDecl>(decl)) {
      const auto *arguments = function->getTemplateSpecializationArgs();
      const auto *primary = function->getPrimaryTemplate();
      if (arguments && primary) {
        // A monolithic USR compresses repeated template types according to
        // earlier arguments. Closure linkage changes can then change backrefs
        // even after replacing a closure's standalone USR. Preserve the exact
        // selected primary and compare canonical structured arguments instead.
        std::string value = "specialization:" + identity(primary) + ':';
        for (const auto &argument : arguments->asArray()) {
          const auto part = template_argument(argument);
          value += std::to_string(part.size()) + ':' + part;
        }
        return value;
      }
    }
    llvm::SmallString<256> usr;
    if (index::generateUSRForDecl(decl, usr)) {
      unsupported = "unresolved_identity";
      return "?";
    }
    const auto stable = normalized(usr.str().str());
    return identities ? mapped_identity(decl, ctx, *identities, stable)
                      : stable;
  }

  void initialize(const FunctionDecl *fn) {
    for (const auto *parameter : fn->parameters()) {
      locals[parameter->getCanonicalDecl()] = next_local++;
    }
    // Lambdas remain unsupported candidates. Residual lambda identities must
    // nevertheless survive collation: Clang embeds their physical locations in
    // type spellings and template specialization USRs. Bind each closure to its
    // lexical ordinal in the corresponding enclosing function body.
    struct Lambdas : RecursiveASTVisitor<Lambdas> {
      Semantics &semantic;
      std::string owner;
      unsigned ordinal = 0;
      Lambdas(Semantics &value, const FunctionDecl *function)
          : semantic(value) {
        owner = semantic.identity(function);
      }
      bool VisitLambdaExpr(LambdaExpr *lambda) {
        const auto stable =
            "closure:" + owner + ':' + std::to_string(ordinal++);
        const auto *record = lambda->getLambdaClass();
        semantic.closures[record->getCanonicalDecl()] = stable;
        llvm::SmallString<256> usr;
        if (!index::generateUSRForDecl(record, usr)) {
          const auto identity = usr.str().str();
          semantic.substitutions.emplace_back(identity, stable);
          if (identity.compare(0, 2, "c:") == 0)
            semantic.substitutions.emplace_back(identity.substr(2), stable);
        }
        llvm::SmallString<256> type_usr;
        if (!index::generateUSRForType(semantic.ctx.getRecordType(record),
                                       semantic.ctx, type_usr)) {
          const auto identity = type_usr.str().str();
          semantic.substitutions.emplace_back(identity, stable);
          if (identity.compare(0, 2, "c:") == 0)
            semantic.substitutions.emplace_back(identity.substr(2), stable);
        }
        semantic.substitutions.emplace_back(
            type_name(semantic.ctx.getRecordType(record), semantic.ctx),
            stable);
        return true;
      }
    } lambdas(*this, fn);
    if (fn->getBody())
      lambdas.TraverseStmt(fn->getBody());
  }

  bool VisitDecl(Decl *decl) {
    if (isa<ParmVarDecl>(decl))
      return true;
    if (const auto *variable = dyn_cast<VarDecl>(decl)) {
      item("variable");
      item(identity(variable));
      item(type_name(variable->getType(), ctx));
      item(std::to_string(variable->getStorageClass()));
      item(std::to_string(variable->getTLSKind()));
      if (const auto *record = ctx.getBaseElementType(variable->getType())
                                   ->getAsCXXRecordDecl()) {
        if (const auto *destructor = record->getDestructor())
          item(identity(destructor));
      }
      if (variable->hasAttrs())
        unsupported = "unsupported_attribute";
    } else if (const auto *alias = dyn_cast<TypedefNameDecl>(decl)) {
      item("local-type");
      item(type_name(alias->getUnderlyingType(), ctx));
    } else if (!isa<FunctionDecl>(decl)) {
      unsupported = "unsupported_local_declaration";
    }
    return true;
  }

  bool VisitStmt(Stmt *stmt) {
    item(stmt->getStmtClassName());
    item(std::to_string(std::distance(stmt->child_begin(), stmt->child_end())));
    // Explicit semantic vocabulary: unknown nodes cause a conservative skip.
    switch (stmt->getStmtClass()) {
#define SH_NODE(name) case Stmt::name##Class:
      SH_NODE(CompoundStmt)
      SH_NODE(DeclStmt)
      SH_NODE(ReturnStmt)
      SH_NODE(IfStmt)
      SH_NODE(ForStmt) SH_NODE(WhileStmt) SH_NODE(DoStmt) SH_NODE(
          SwitchStmt) SH_NODE(CaseStmt) SH_NODE(DefaultStmt) SH_NODE(BreakStmt)
          SH_NODE(ContinueStmt) SH_NODE(NullStmt) SH_NODE(IntegerLiteral) SH_NODE(
              FloatingLiteral) SH_NODE(CharacterLiteral) SH_NODE(StringLiteral)
              SH_NODE(CXXBoolLiteralExpr) SH_NODE(CXXNullPtrLiteralExpr) SH_NODE(
                  DeclRefExpr) SH_NODE(MemberExpr) SH_NODE(CXXThisExpr) SH_NODE(ParenExpr)
                  SH_NODE(UnaryOperator) SH_NODE(BinaryOperator) SH_NODE(
                      CompoundAssignOperator) SH_NODE(ConditionalOperator)
                      SH_NODE(ArraySubscriptExpr) SH_NODE(CallExpr) SH_NODE(
                          CXXMemberCallExpr) SH_NODE(CXXOperatorCallExpr)
                          SH_NODE(ImplicitCastExpr) SH_NODE(CStyleCastExpr) SH_NODE(
                              CXXStaticCastExpr) SH_NODE(CXXConstCastExpr)
                              SH_NODE(CXXReinterpretCastExpr) SH_NODE(
                                  CXXFunctionalCastExpr) SH_NODE(CXXConstructExpr)
                                  SH_NODE(CXXTemporaryObjectExpr) SH_NODE(
                                      MaterializeTemporaryExpr) SH_NODE(CXXBindTemporaryExpr)
                                      SH_NODE(ExprWithCleanups) SH_NODE(
                                          InitListExpr) SH_NODE(ImplicitValueInitExpr)
                                          SH_NODE(CXXScalarValueInitExpr) SH_NODE(
                                              CXXStdInitializerListExpr) SH_NODE(CXXDefaultArgExpr)
                                              SH_NODE(CXXDefaultInitExpr) SH_NODE(
                                                  UnaryExprOrTypeTraitExpr)
                                                  SH_NODE(CXXNoexceptExpr) SH_NODE(
                                                      CXXThrowExpr) SH_NODE(CXXTryStmt)
                                                      SH_NODE(CXXCatchStmt) SH_NODE(
                                                          CXXForRangeStmt)
                                                          SH_NODE(ArrayInitLoopExpr) SH_NODE(
                                                              ArrayInitIndexExpr)
                                                              SH_NODE(
                                                                  OpaqueValueExpr)
                                                                  SH_NODE(
                                                                      SubstNonTypeTemplateParmExpr) break;
#undef SH_NODE
    default:
      unsupported = std::string("unsupported_ast:") + stmt->getStmtClassName();
    }
    if (const auto *expr = dyn_cast<Expr>(stmt)) {
      item(type_name(expr->getType(), ctx));
      item(std::to_string(expr->getValueKind()));
      item(std::to_string(expr->getObjectKind()));
      if (!expr->isTypeDependent())
        item(std::to_string(
            expr->getFPFeaturesInEffect(ctx.getLangOpts()).getAsOpaqueInt()));
      if (expr->isTypeDependent() || expr->isValueDependent())
        unsupported = "dependent_expression";
    }
    if (const auto *reference = dyn_cast<DeclRefExpr>(stmt))
      item(identity(reference->getDecl()));
    if (const auto *reference = dyn_cast<DeclRefExpr>(stmt)) {
      if (const auto *enumerator =
              dyn_cast<EnumConstantDecl>(reference->getDecl())) {
        llvm::SmallString<40> value;
        enumerator->getInitVal().toString(value, 10);
        item(value.str().str());
      }
    }
    if (const auto *member = dyn_cast<MemberExpr>(stmt)) {
      item(identity(member->getMemberDecl()));
      item(member->isArrow() ? "arrow" : "dot");
    }
    if (const auto *call = dyn_cast<CallExpr>(stmt)) {
      if (call->getDirectCallee())
        item(identity(call->getDirectCallee()));
      else
        unsupported = "indirect_call";
    }
    if (const auto *construction = dyn_cast<CXXConstructExpr>(stmt)) {
      item(identity(construction->getConstructor()));
      item(std::to_string(
          static_cast<unsigned>(construction->getConstructionKind())));
      item(construction->isElidable() ? "elidable" : "nonelidable");
    }
    if (const auto *temporary = dyn_cast<CXXBindTemporaryExpr>(stmt))
      item(identity(temporary->getTemporary()->getDestructor()));
    if (const auto *cast = dyn_cast<CastExpr>(stmt))
      item(cast->getCastKindName());
    if (const auto *unary = dyn_cast<UnaryOperator>(stmt))
      item(UnaryOperator::getOpcodeStr(unary->getOpcode()).str());
    if (const auto *binary = dyn_cast<BinaryOperator>(stmt))
      item(BinaryOperator::getOpcodeStr(binary->getOpcode()).str());
    if (const auto *literal = dyn_cast<IntegerLiteral>(stmt)) {
      llvm::SmallString<40> value;
      literal->getValue().toString(value, 10, false);
      item(value.str().str());
    }
    if (const auto *literal = dyn_cast<FloatingLiteral>(stmt)) {
      llvm::SmallString<40> value;
      literal->getValue().toString(value);
      item(value.str().str());
    }
    if (const auto *literal = dyn_cast<CharacterLiteral>(stmt))
      item(std::to_string(literal->getValue()));
    if (const auto *literal = dyn_cast<CXXBoolLiteralExpr>(stmt))
      item(literal->getValue() ? "true" : "false");
    if (const auto *literal = dyn_cast<StringLiteral>(stmt))
      item(literal->getBytes().str());
    if (const auto *trait = dyn_cast<UnaryExprOrTypeTraitExpr>(stmt)) {
      item(std::to_string(trait->getKind()));
      item(type_name(trait->getTypeOfArgument(), ctx));
      Expr::EvalResult result;
      if (trait->EvaluateAsInt(result, ctx)) {
        llvm::SmallString<40> value;
        result.Val.getInt().toString(value, 10);
        item(value.str().str());
      } else
        unsupported = "unevaluated_trait";
    }
    if (const auto *expr = dyn_cast<CXXNoexceptExpr>(stmt))
      item(expr->getValue() ? "true" : "false");
    return true;
  }

  bool TraverseCXXDefaultArgExpr(CXXDefaultArgExpr *expr) {
    VisitStmt(expr);
    return TraverseStmt(expr->getExpr());
  }
  bool TraverseCXXDefaultInitExpr(CXXDefaultInitExpr *expr) {
    VisitStmt(expr);
    return TraverseStmt(expr->getExpr());
  }
};
std::string mapped_identity(const Decl *decl, ASTContext &ctx,
                            IdentityIndex &index, const std::string &usr) {
  const auto *value = dyn_cast<ValueDecl>(decl);
  if (!value || (value->getLinkageInternal() != Linkage::Internal &&
                 value->getLinkageInternal() != Linkage::UniqueExternal))
    return usr;
  const auto &sm = ctx.getSourceManager();
  const auto physical = SourceRange(sm.getExpansionLoc(decl->getBeginLoc()),
                                    sm.getExpansionLoc(decl->getEndLoc()));
  const auto range = span(physical, sm, ctx.getLangOpts());
  if (!range.valid() ||
      (!index.files.count(range.file) && range.file != index.artifact))
    return usr;
  const auto text =
      Lexer::getSourceText(CharSourceRange::getTokenRange(physical), sm,
                           ctx.getLangOpts())
          .str();
  std::string signature = std::string(decl->getDeclKindName()) + ':' +
                          value->getQualifiedNameAsString() + ':' +
                          type_name(value->getType(), ctx) + ':';
  for (const auto &token : raw_tokens(text))
    signature += std::to_string(token.text.size()) + ':' + token.text;
  if (range.file != index.artifact) {
    const auto origin =
        "origin:" + range.file + ':' + std::to_string(range.begin);
    if (!index.frozen)
      index.origins[signature].insert(origin);
    return origin;
  }
  const auto found = index.origins.find(signature);
  if (found == index.origins.end() || found->second.size() != 1)
    return "unresolved-origin:" + usr;
  return *found->second.begin();
}
} // namespace

std::string declaration_identity(const Decl *decl, ASTContext &ctx,
                                  IdentityIndex &identities) {
  Semantics semantic(ctx, &identities);
  return semantic.identity(decl);
}
std::string function_type(FunctionDecl *fn, ASTContext &ctx,
                           IdentityIndex &identities) {
  Semantics semantic(ctx, &identities);
  semantic.initialize(fn);
  return semantic.normalized(type_name(fn->getType(), ctx));
}
Record function_semantics(FunctionDecl *fn, ASTContext &ctx,
                           IdentityIndex &identities) {
  Semantics semantic(ctx, &identities);
  semantic.initialize(fn);
  semantic.TraverseStmt(fn->getBody());
  return {semantic.out.str(), {}, semantic.unsupported,
          semantic.internal_dependency};
}
std::string variable_initializer(VarDecl *variable, ASTContext &ctx,
                                  IdentityIndex &identities) {
  Semantics semantic(ctx, &identities);
  semantic.item(type_name(variable->getType(), ctx));
  semantic.TraverseStmt(variable->getInit());
  return semantic.out.str();
}
std::string field_initializer(FieldDecl *field, ASTContext &ctx,
                               IdentityIndex &identities) {
  Semantics semantic(ctx, &identities);
  semantic.TraverseStmt(field->getInClassInitializer());
  return semantic.out.str();
}
std::string default_argument(FunctionDecl *fn, unsigned parameter,
                              ASTContext &ctx, IdentityIndex &identities) {
  Semantics semantic(ctx, &identities);
  semantic.initialize(fn);
  semantic.TraverseStmt(fn->getParamDecl(parameter)->getDefaultArg());
  return semantic.out.str();
}

} // namespace single_header::stitching
