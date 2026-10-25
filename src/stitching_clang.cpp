#include "helpers.hpp"
#include "stitching.hpp"
#include "clang_sdk_allocator.hpp"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Attr.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/RecordLayout.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Index/USRGeneration.h>
#include <clang/Lex/Lexer.h>
#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/JSONCompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/raw_ostream.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;
using namespace clang;
using json = nlohmann::json;

namespace single_header {
namespace {

std::string read_bytes(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("cannot read stitching input: " + path.string());
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

struct Span {
  std::string file;
  unsigned begin = 0, end = 0;
  bool valid() const { return !file.empty() && begin < end; }
};

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

struct IdentityIndex {
  std::set<std::string> files;
  std::string artifact;
  std::map<std::string, std::set<std::string>> origins;
  bool frozen = false;
};
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

struct TokenInfo {
  std::string file, spelling;
  unsigned offset;
  bool macro;
};
struct Expansion {
  Span where;
  std::string name;
  bool annotation;
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
};
struct Environment {
  std::set<std::string> headers, sources;
  std::map<std::string, std::string> input;
  std::set<std::string> annotations;
  mutable IdentityIndex identities;
  std::string artifact;
};

// Only a canonical whole-file #ifndef/#define/.../#endif is transparent.
struct PhysicalToken {
  std::string text;
  unsigned offset;
  bool start;
};
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
struct Directive {
  std::vector<std::string> words;
  unsigned begin, end;
  size_t first, last;
};
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

struct Collector : RecursiveASTVisitor<Collector> {
  ASTContext &ctx;
  ParseResult &result;
  const Environment &env;
  const std::vector<TokenInfo> &tokens;
  const std::vector<Expansion> &expansions;
  bool discover;
  Collector(ASTContext &context, ParseResult &output,
            const Environment &environment,
            const std::vector<TokenInfo> &input_tokens,
            const std::vector<Expansion> &macro_expansions, bool discovery)
      : ctx(context), result(output), env(environment), tokens(input_tokens),
        expansions(macro_expansions), discover(discovery) {}

  bool project(SourceLocation loc) const {
    const auto file = file_name(loc, ctx.getSourceManager());
    return file == env.artifact || env.headers.count(file) ||
           env.sources.count(file);
  }
  std::string id(const Decl *decl) {
    Semantics semantic(ctx, &env.identities);
    return semantic.identity(decl);
  }
  Span definition_span(FunctionDecl *fn) const {
    auto range = fn->getSourceRange();
    if (range.getBegin().isMacroID()) {
      const auto &sm = ctx.getSourceManager();
      const auto file = file_name(range.getBegin(), sm);
      const auto offset =
          sm.getFileOffset(sm.getExpansionLoc(range.getBegin()));
      bool allowed = false;
      for (const auto &expansion : expansions)
        if (expansion.where.file == file && expansion.where.begin == offset &&
            expansion.annotation)
          allowed = true;
      if (!allowed)
        return {};
      range.setBegin(sm.getExpansionLoc(range.getBegin()));
    }
    return span(range, ctx.getSourceManager(), ctx.getLangOpts());
  }
  Record record(FunctionDecl *fn) {
    Semantics semantic(ctx, &env.identities);
    semantic.initialize(fn);
    semantic.TraverseStmt(fn->getBody());
    Record record{semantic.out.str(),
                  {},
                  semantic.unsupported,
                  semantic.internal_dependency};
    const auto range = span(fn->getBody()->getSourceRange(),
                            ctx.getSourceManager(), ctx.getLangOpts());
    for (const auto &token : tokens) {
      if (token.file == range.file && token.offset >= range.begin &&
          token.offset < range.end) {
        record.tokens +=
            std::to_string(token.spelling.size()) + ':' + token.spelling + ';';
        if (token.macro && record.unsupported.empty())
          record.unsupported = "body_macro";
      }
    }
    for (const auto &expansion : expansions) {
      if (expansion.where.file == range.file &&
          expansion.where.begin >= range.begin &&
          expansion.where.begin < range.end && record.unsupported.empty())
        record.unsupported = "body_macro";
    }
    return record;
  }
  bool VisitVarDecl(VarDecl *variable) {
    if (!variable->isFileVarDecl() || !project(variable->getLocation()))
      return true;
    if (env.sources.count(
            file_name(variable->getLocation(), ctx.getSourceManager())) &&
        variable->isThisDeclarationADefinition() != VarDecl::DeclarationOnly)
      result.source_state = true;
    result.declarations["variable-type:" + id(variable)] =
        type_name(variable->getType(), ctx);
    if (!variable->hasInit())
      return true;
    Semantics semantic(ctx, &env.identities);
    semantic.item(type_name(variable->getType(), ctx));
    semantic.TraverseStmt(variable->getInit());
    result.declarations["variable:" + id(variable)] = semantic.out.str();
    return true;
  }
  bool VisitCXXRecordDecl(CXXRecordDecl *record) {
    if (!record->isCompleteDefinition() || record->isDependentContext() ||
        !record->getIdentifier() || !project(record->getLocation()))
      return true;
    const auto &layout = ctx.getASTRecordLayout(record);
    std::string value = std::to_string(layout.getSize().getQuantity()) + ':' +
                        std::to_string(layout.getAlignment().getQuantity());
    unsigned field_index = 0;
    for (const auto *field : record->fields()) {
      value += ':' + type_name(field->getType(), ctx) + ':' +
               std::to_string(layout.getFieldOffset(field_index++));
    }
    result.declarations["layout:" + id(record)] = std::move(value);
    return true;
  }
  bool VisitFieldDecl(FieldDecl *field) {
    if (!field->hasInClassInitializer() || !project(field->getLocation()))
      return true;
    Semantics semantic(ctx, &env.identities);
    semantic.TraverseStmt(field->getInClassInitializer());
    result.declarations["field:" + id(field)] = semantic.out.str();
    return true;
  }
  bool VisitFunctionDecl(FunctionDecl *fn) {
    if (!project(fn->getLocation()) || fn->isImplicit())
      return true;
    if (const auto *method = dyn_cast<CXXMethodDecl>(fn))
      if (method->getParent()->isLambda())
        return true; // Included in the enclosing body's record.
    const auto key = id(fn);
    result.declarations["type:" + key] = type_name(fn->getType(), ctx);
    std::string attributes;
    llvm::raw_string_ostream attribute_output(attributes);
    for (const auto *attribute : fn->attrs())
      attribute->printPretty(attribute_output,
                             PrintingPolicy(ctx.getLangOpts()));
    result.declarations["attributes:" + key] = attributes;
    for (unsigned i = 0; i < fn->getNumParams(); ++i) {
      if (fn->getParamDecl(i)->hasDefaultArg() &&
          !fn->getParamDecl(i)->hasUnparsedDefaultArg()) {
        Semantics semantic(ctx, &env.identities);
        semantic.initialize(fn);
        semantic.TraverseStmt(fn->getParamDecl(i)->getDefaultArg());
        result.declarations["default:" + key + ':' + std::to_string(i)] =
            semantic.out.str();
      }
    }
    if (!fn->doesThisDeclarationHaveABody())
      return true;
    Record semantic;
    if (fn->getBody()) {
      semantic = record(fn);
      const auto existing = result.bodies.find(key);
      if (existing != result.bodies.end() && !(existing->second == semantic))
        result.errors = true;
      result.bodies[key] = semantic;
    } else {
      // Delayed template parsing (the Windows default) and defaulted bodies
      // can report a definition without an instantiated statement tree.
      semantic.unsupported = "unparsed_function_body";
    }
    if (!discover || !env.sources.count(
                         file_name(fn->getLocation(), ctx.getSourceManager())))
      return true;
    Candidate candidate;
    candidate.id = key;
    candidate.name = fn->getQualifiedNameAsString();
    candidate.file = file_name(fn->getLocation(), ctx.getSourceManager());
    candidate.line =
        ctx.getSourceManager().getSpellingLineNumber(fn->getLocation());
    candidate.method = isa<CXXMethodDecl>(fn);
    candidate.already_inline = fn->isInlined();
    for (const auto *redeclaration : fn->redecls()) {
      if (!redeclaration->doesThisDeclarationHaveABody() &&
          env.headers.count(
              file_name(redeclaration->getLocation(), ctx.getSourceManager())))
        candidate.matched = true;
    }
    candidate.reason = classify(fn, semantic);
    if (candidate.reason.empty())
      plan(fn, candidate);
    result.candidates.push_back(std::move(candidate));
    return true;
  }
  std::string classify(FunctionDecl *fn, const Record &semantic) {
    if (fn->getTemplatedKind() != FunctionDecl::TK_NonTemplate ||
        fn->isDependentContext())
      return "template";
    if (fn->isOverloadedOperator() || isa<CXXConstructorDecl>(fn) ||
        isa<CXXDestructorDecl>(fn) || isa<CXXConversionDecl>(fn))
      return "special_function";
    if (fn->isConstexpr() || fn->isConsteval() || fn->isVariadic() ||
        fn->isExternC())
      return "unsupported_function";
    if (fn->getLinkageInternal() != Linkage::External)
      return "internal_linkage";
    if (fn->hasAttrs())
      return "unsupported_attribute";
    if (const auto *method = dyn_cast<CXXMethodDecl>(fn)) {
      if (method->isVirtual())
        return "virtual_method";
      if (method->getParent()->getDescribedClassTemplate() ||
          isa<ClassTemplateSpecializationDecl>(method->getParent()))
        return "template";
    }
    if (!semantic.unsupported.empty())
      return semantic.unsupported;
    if (semantic.internal)
      return "internal_dependency";
    if (!definition_span(fn).valid())
      return "macro_or_invalid_range";
    const auto range = definition_span(fn);
    const auto &text = env.input.at(range.file);
    if (conditional_range(text, range.begin, range.end))
      return "conditional_region";
    const auto physical_tokens = raw_tokens(text);
    for (const auto &directive :
         directives(physical_tokens, static_cast<unsigned>(text.size()))) {
      if (directive.begin >= range.begin && directive.begin < range.end)
        return "body_directive";
    }
    for (const auto &token : physical_tokens) {
      if (token.offset >= range.begin && token.offset < range.end &&
          (token.text == "_Pragma" || token.text == "__pragma"))
        return "body_directive";
    }
    return {};
  }
  void plan(FunctionDecl *fn, Candidate &candidate) {
    const auto &sm = ctx.getSourceManager();
    FunctionDecl *declaration = nullptr;
    Span destination;
    for (auto *redecl : fn->redecls()) {
      if (redecl->doesThisDeclarationHaveABody() ||
          !env.headers.count(file_name(redecl->getLocation(), sm)))
        continue;
      auto range = span(redecl->getSourceRange(), sm, ctx.getLangOpts());
      if (!range.valid())
        continue;
      if (!declaration || std::tie(range.file, range.begin) >
                              std::tie(destination.file, destination.begin)) {
        declaration = redecl;
        destination = range;
      }
    }
    if (!declaration) {
      candidate.reason = "no_emitted_declaration";
      return;
    }
    if (declaration->hasAttrs()) {
      candidate.reason = "unsupported_attribute";
      return;
    }
    const auto &header = env.input.at(destination.file);
    for (const auto &macro : expansions) {
      if (macro.where.file == destination.file &&
          macro.where.end <= destination.begin &&
          raw_tokens(header.substr(macro.where.end,
                                   destination.begin - macro.where.end))
              .empty()) {
        candidate.reason = "declaration_macro";
        return;
      }
    }
    if (conditional_range(header, destination.begin, destination.end)) {
      candidate.reason = "conditional_region";
      return;
    }
    auto next =
        Lexer::findNextToken(declaration->getEndLoc(), sm, ctx.getLangOpts());
    if (!next || !next->is(tok::semi)) {
      candidate.reason = "declaration_range";
      return;
    }
    destination.end = sm.getFileOffset(next->getLocation()) + 1;
    std::string signature = header.substr(
        destination.begin, destination.end - destination.begin - 1);
    struct Rename {
      unsigned begin, length;
      std::string value;
    };
    std::vector<Rename> names;
    for (unsigned i = 0; i < fn->getNumParams(); ++i) {
      const auto *source = fn->getParamDecl(i);
      const auto *target = declaration->getParamDecl(i);
      if (source->hasAttrs() || target->hasAttrs() ||
          target->getLocation().isMacroID()) {
        candidate.reason = "parameter_range";
        return;
      }
      const auto name = source->getNameAsString();
      if (name == target->getNameAsString())
        continue;
      if (name.empty()) {
        candidate.reason = "unnamed_definition_parameter";
        return;
      }
      if (target->getIdentifier()) {
        const auto where =
            span(SourceRange(target->getLocation()), sm, ctx.getLangOpts());
        if (!where.valid() || where.file != destination.file) {
          candidate.reason = "parameter_range";
          return;
        }
        names.push_back(
            {where.begin - destination.begin, where.end - where.begin, name});
      } else {
        if (target->getType()->isFunctionPointerType() ||
            target->getType()->isArrayType()) {
          candidate.reason = "unnamed_complex_parameter";
          return;
        }
        const auto where =
            span(target->getTypeSourceInfo()->getTypeLoc().getSourceRange(), sm,
                 ctx.getLangOpts());
        if (!where.valid() || where.file != destination.file) {
          candidate.reason = "parameter_range";
          return;
        }
        names.push_back({where.end - destination.begin, 0, " " + name});
      }
    }
    std::sort(names.begin(), names.end(), [](const Rename &a, const Rename &b) {
      return a.begin > b.begin;
    });
    for (const auto &name : names)
      signature.replace(name.begin, name.length, name.value);
    const auto body_range =
        span(fn->getBody()->getSourceRange(), sm, ctx.getLangOpts());
    auto source_range = definition_span(fn);
    if (!body_range.valid() || !source_range.valid()) {
      candidate.reason = "macro_or_invalid_range";
      return;
    }
    const auto &source = env.input.at(source_range.file);
    // Empty annotation expansions may precede AST begin locations. Include
    // their complete spelling range in the paired source removal.
    for (auto macro = expansions.rbegin(); macro != expansions.rend();
         ++macro) {
      if (macro->where.file != source_range.file)
        continue;
      if (macro->where.end <= source_range.begin &&
          raw_tokens(source.substr(macro->where.end,
                                   source_range.begin - macro->where.end))
              .empty()) {
        if (!macro->annotation) {
          candidate.reason = "signature_macro";
          return;
        }
        source_range.begin = macro->where.begin;
      }
    }
    for (const auto &macro : expansions) {
      if (macro.where.file == source_range.file &&
          macro.where.begin >= source_range.begin &&
          macro.where.begin < body_range.begin && !macro.annotation) {
        candidate.reason = "signature_macro";
        return;
      }
      if (macro.where.file == destination.file &&
          macro.where.begin >= destination.begin &&
          macro.where.begin < destination.end) {
        candidate.reason = "declaration_macro";
        return;
      }
    }
    const auto body =
        source.substr(body_range.begin, body_range.end - body_range.begin);
    candidate.declaration = {
        destination,
        (candidate.method || declaration->isInlineSpecified() ? ""
                                                              : "inline ") +
            signature + " " + body};
    candidate.definition = {source_range, ""};
  }
};

struct Diagnostics : DiagnosticConsumer {
  ParseResult &result;
  explicit Diagnostics(ParseResult &output) : result(output) {}
  void HandleDiagnostic(DiagnosticsEngine::Level level,
                        const Diagnostic &info) override {
    if (level >= DiagnosticsEngine::Error)
      result.errors = true;
    if (level >= DiagnosticsEngine::Warning &&
        result.diagnostics.size() < 12000) {
      llvm::SmallString<256> message;
      info.FormatDiagnostic(message);
      result.diagnostics += message.str().str() + '\n';
    }
  }
};
struct Action : ASTFrontendAction {
  ParseResult &result;
  const Environment &env;
  std::vector<TokenInfo> tokens;
  std::vector<Expansion> expansions;
  bool discover;
  Action(ParseResult &output, const Environment &environment, bool discovery)
      : result(output), env(environment), discover(discovery) {}
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &ci,
                                                 llvm::StringRef) override {
    if (!ci.getLangOpts().CPlusPlus17) {
      result.errors = true;
      result.diagnostics += "stitching requires C++17 or later\n";
    }
    struct Macros : PPCallbacks {
      Action &action;
      CompilerInstance &ci;
      Macros(Action &owner, CompilerInstance &compiler)
          : action(owner), ci(compiler) {}
      void FileChanged(SourceLocation location, FileChangeReason reason,
                       SrcMgr::CharacteristicKind, FileID) override {
        if (reason == EnterFile &&
            file_name(location, ci.getSourceManager()) == action.env.artifact)
          action.result.artifact_seen = true;
      }
      void MacroExpands(const Token &name, const MacroDefinition &definition,
                        SourceRange range, const MacroArgs *) override {
        const auto &sm = ci.getSourceManager();
        const auto location = sm.getExpansionLoc(range.getBegin());
        const auto end = Lexer::getLocForEndOfToken(
            sm.getExpansionLoc(range.getEnd()), 0, sm, ci.getLangOpts());
        if (location.isInvalid() || end.isInvalid())
          return;
        const auto spelling = ci.getPreprocessor().getSpelling(name);
        const auto *info = definition.getMacroInfo();
        const bool annotation = action.env.annotations.count(spelling) &&
                                info && !info->isFunctionLike() &&
                                (info->getNumTokens() == 0 ||
                                 (info->getNumTokens() == 1 &&
                                  ci.getPreprocessor().getSpelling(
                                      *info->tokens_begin()) == "inline"));
        action.expansions.push_back(
            {{file_name(location, sm), sm.getFileOffset(location),
              sm.getFileOffset(end)},
             spelling,
             annotation});
      }
    };
    ci.getPreprocessor().addPPCallbacks(std::make_unique<Macros>(*this, ci));
    ci.getPreprocessor().setTokenWatcher([this, &ci](const Token &token) {
      if (token.is(tok::eof) || token.getLocation().isInvalid())
        return;
      const auto &sm = ci.getSourceManager();
      const auto file = file_name(token.getLocation(), sm);
      if (file == env.artifact)
        result.artifact_seen = true;
      if (file != env.artifact && !env.headers.count(file) &&
          !env.sources.count(file))
        return;
      tokens.push_back(
          {file, ci.getPreprocessor().getSpelling(token),
           sm.getFileOffset(sm.getExpansionLoc(token.getLocation())),
           token.getLocation().isMacroID()});
    });
    struct Consumer : ASTConsumer {
      Action &action;
      explicit Consumer(Action &owner) : action(owner) {}
      void HandleTranslationUnit(ASTContext &ctx) override {
        // Recovery ASTs can contain null types and unresolved declarations.
        // A failed parse is already a rejection; never analyze recovery nodes.
        if (action.result.errors || ctx.getDiagnostics().hasErrorOccurred())
          return;
        Collector collector(ctx, action.result, action.env, action.tokens,
                            action.expansions, action.discover);
        collector.TraverseDecl(ctx.getTranslationUnitDecl());
      }
    };
    return std::make_unique<Consumer>(*this);
  }
};
struct Factory : tooling::FrontendActionFactory {
  ParseResult &result;
  const Environment &env;
  bool discover;
  Factory(ParseResult &output, const Environment &environment, bool discovery)
      : result(output), env(environment), discover(discovery) {}
  std::unique_ptr<FrontendAction> create() override {
    return std::make_unique<Action>(result, env, discover);
  }
};
struct Database : tooling::CompilationDatabase {
  tooling::CompileCommand command;
  explicit Database(tooling::CompileCommand value)
      : command(std::move(value)) {}
  std::vector<tooling::CompileCommand>
  getCompileCommands(llvm::StringRef) const override {
    return {command};
  }
};
ParseResult parse(const tooling::CompileCommand &command,
                  const Environment &env, const std::string *artifact) {
  ParseResult result;
  Database database(command);
  tooling::ClangTool tool(database, {command.Filename});
  Diagnostics diagnostics(result);
  tool.setDiagnosticConsumer(&diagnostics);
  for (const auto &[path, text] : env.input) {
    if (artifact && paths_alias(path, command.Filename))
      continue;
    tool.mapVirtualFile(path, text);
  }
  if (artifact) {
    tool.mapVirtualFile(env.artifact, *artifact);
    const auto include = "#include " + json(env.artifact).dump() + "\n";
    tool.mapVirtualFile(command.Filename, include);
    Factory factory(result, env, false);
    if (tool.run(&factory))
      result.errors = true;
    if (!result.artifact_seen) {
      result.errors = true;
      result.diagnostics +=
          "generated artifact was not entered during destination validation\n";
    }
  } else {
    Factory factory(result, env, true);
    if (tool.run(&factory))
      result.errors = true;
  }
  return result;
}
std::string compare(const ParseResult &before, const ParseResult &after,
                    std::string *detail = nullptr, bool exact = true) {
  if (after.errors)
    return "destination_parse_error";
  for (const auto &[key, record] : before.bodies) {
    const auto found = after.bodies.find(key);
    if (found == after.bodies.end())
      return "missing_definition:" + key;
    if (!(record == found->second)) {
      if (detail) {
        const auto &a = record.semantics == found->second.semantics
                            ? record.tokens
                            : record.semantics;
        const auto &b = record.semantics == found->second.semantics
                            ? found->second.tokens
                            : found->second.semantics;
        size_t index = 0;
        while (index < a.size() && index < b.size() && a[index] == b[index])
          ++index;
        const auto start = index > 80 ? index - 80 : 0;
        *detail = "first differing record at " + std::to_string(index) +
                  "\noriginal: " + a.substr(start, 320) +
                  "\ngenerated: " + b.substr(start, 320) + '\n';
      }
      return "changed_semantics:" + key;
    }
  }
  for (const auto &[key, record] : before.declarations) {
    const auto found = after.declarations.find(key);
    if (found == after.declarations.end() || record != found->second) {
      if (detail)
        *detail =
            "declaration record " + key + "\noriginal: " + record +
            "\ngenerated: " +
            (found == after.declarations.end() ? "<missing>" : found->second) +
            '\n';
      return "changed_declaration:" + key;
    }
  }
  if (exact) {
    for (const auto &[key, record] : after.bodies) {
      (void)record;
      if (!before.bodies.count(key))
        return "added_definition:" + key;
    }
    for (const auto &[key, record] : after.declarations) {
      (void)record;
      if (!before.declarations.count(key))
        return "added_declaration:" + key;
    }
  }
  return {};
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

} // namespace

std::string stitch_functions(const Options &options,
                             const std::vector<fs::path> &headers,
                             const std::vector<fs::path> &sources,
                             const RenderArtifact &render) {
  const auto started = std::chrono::steady_clock::now();
  llvm::sys::PrintStackTraceOnErrorSignal("single_header");
  Environment env;
  env.annotations.insert(options.inline_annotations.begin(),
                         options.inline_annotations.end());
  for (const auto &header : headers)
    env.headers.insert(normalize_path(header).generic_string());
  for (const auto &source : sources)
    env.sources.insert(normalize_path(source).generic_string());
  for (const auto &path : env.headers)
    env.input[path] = read_bytes(path);
  for (const auto &path : env.sources)
    env.input[path] = read_bytes(path);
  env.artifact = normalize_path(fs::current_path() /
                                "__single_header_stitching_artifact.hpp")
                     .generic_string();
  env.identities.files.insert(env.headers.begin(), env.headers.end());
  env.identities.files.insert(env.sources.begin(), env.sources.end());
  env.identities.artifact = env.artifact;
  if (env.input.count(env.artifact) || fs::exists(env.artifact))
    throw std::runtime_error("reserved analysis path already exists: " +
                             env.artifact);
  for (const auto &[path, text] : env.input) {
    (void)text;
    if (paths_alias(options.stitching_report, path) ||
        paths_alias(options.output, path))
      throw std::runtime_error(
          "stitching output/report collides with an input: " + path);
  }
  std::unique_ptr<tooling::CompilationDatabase> database;
  if (!options.compilation_database.empty()) {
    auto path = options.compilation_database;
    if (fs::is_directory(path))
      path /= "compile_commands.json";
    std::string error;
    database = tooling::JSONCompilationDatabase::loadFromFile(
        path.string(), error, tooling::JSONCommandLineSyntax::AutoDetect);
    if (!database)
      throw std::runtime_error("cannot load compilation database: " + error);
  }
  std::vector<tooling::CompileCommand> commands;
  for (const auto &source : sources) {
    if (database) {
      auto matches = database->getCompileCommands(source.string());
      if (matches.size() != 1)
        throw std::runtime_error("expected exactly one compile command for " +
                                 source.string() + "; found " +
                                 std::to_string(matches.size()));
      auto command = std::move(matches.front());
      command.Filename = normalize_path(source).string();
      commands.push_back(std::move(command));
    } else {
      std::vector<std::string> arguments{SINGLE_HEADER_CLANG_BINARY};
      arguments.insert(arguments.end(), options.frontend_args.begin(),
                       options.frontend_args.end());
      arguments.push_back(source.string());
      commands.emplace_back(fs::current_path().string(), source.string(),
                            arguments, "");
    }
  }
  json report = {{"version", 1},
                 {"backend", "clang-20"},
                 {"mode", options.function_stitching},
                 {"configurations", json::array()},
                 {"candidates", json::array()},
                 {"baseline", "valid"}};
  report["baselineDifferences"] = json::array();
  std::set<std::string> baseline_difference_keys;
  std::vector<ParseResult> originals, baselines;
  std::map<std::string, Candidate> candidates;
  const auto legacy = render({});
  std::string baseline_problem, baseline_diagnostics;
  bool pragma = false;
  for (const auto &[path, text] : env.input) {
    (void)path;
    for (const auto &command :
         directives(raw_tokens(text), static_cast<unsigned>(text.size()))) {
      if (!command.words.empty() &&
          (command.words[0] == "line" ||
           (command.words[0] == "pragma" &&
            (command.words.size() < 2 ||
             (command.words[1] != "once" && command.words[1] != "region" &&
              command.words[1] != "endregion" &&
              command.words[1] != "warning" &&
              !((command.words[1] == "clang" || command.words[1] == "GCC") &&
                command.words.size() > 2 &&
                command.words[2] == "diagnostic"))))))
        pragma = true;
    }
  }
  for (const auto &command : commands) {
    report["configurations"].push_back({{"source", command.Filename},
                                        {"directory", command.Directory},
                                        {"arguments", command.CommandLine}});
    auto original = parse(command, env, nullptr);
    if (original.errors)
      throw std::runtime_error("original analysis failed for " +
                               command.Filename + ":\n" + original.diagnostics);
    for (auto &candidate : original.candidates) {
      auto [found, inserted] = candidates.emplace(candidate.id, candidate);
      if (!inserted && (found->second.file != candidate.file ||
                        found->second.definition.where.begin !=
                            candidate.definition.where.begin))
        found->second.reason = "ambiguous_definition";
    }
    originals.push_back(std::move(original));
  }
  // Populate every original-file provenance entry before recording generated
  // identities. A later TU may introduce an internal declaration absent from an
  // earlier TU, but present in the artifact seen by every context.
  env.identities.frozen = true;
  for (unsigned i = 0; i < commands.size(); ++i) {
    const auto &command = commands[i];
    const auto &original = originals[i];
    auto baseline = parse(command, env, &legacy);
    // Original TUs discover/classify candidates. Only the generated, unstitched
    // artifact is the relocation baseline: collation can already change the
    // program before any body is moved. Record those differences separately.
    if (!baseline.errors) {
      for (const auto &[key, record] : original.bodies) {
        if (report["baselineDifferences"].size() >= 10)
          break;
        ParseResult single;
        single.bodies.emplace(key, record);
        std::string detail;
        const auto difference = compare(single, baseline, &detail, false);
        if (!difference.empty() &&
            baseline_difference_keys.insert(difference).second)
          report["baselineDifferences"].push_back(
              {{"source", command.Filename}, {"reason", difference},
               {"detail", detail}, {"severity", "informational"}});
      }
      for (const auto &[key, record] : original.declarations) {
        if (report["baselineDifferences"].size() >= 10)
          break;
        ParseResult single;
        single.declarations.emplace(key, record);
        std::string detail;
        const auto difference = compare(single, baseline, &detail, false);
        if (!difference.empty() &&
            baseline_difference_keys.insert(difference).second)
          report["baselineDifferences"].push_back(
              {{"source", command.Filename}, {"reason", difference},
               {"detail", detail}, {"severity", "informational"}});
      }
    }
    if (baseline.errors && baseline_problem.empty()) {
      baseline_problem = "destination_parse_error";
      baseline_diagnostics = baseline.diagnostics;
    }
    baselines.push_back(std::move(baseline));
  }
  if (!baseline_problem.empty()) {
    report["baseline"] = "invalid";
    report["baselineReason"] = baseline_problem;
    report["baselineDiagnostics"] = baseline_diagnostics;
    std::cerr << "single_header: stitching baseline limitation: "
              << baseline_problem << '\n';
  }
  std::vector<const Candidate *> accepted;
  std::string output = legacy;
  unsigned matched = 0, accepted_free = 0, accepted_methods = 0,
           already_inline = 0, removed_lines = 0;
  for (auto &[key, candidate] : candidates) {
    if (candidate.matched)
      ++matched;
    if (candidate.reason.empty() && options.function_stitching != "all" &&
        candidate.method != (options.function_stitching == "methods"))
      candidate.reason = "mode_excluded";
    if (candidate.reason.empty() && pragma)
      candidate.reason = "unsupported_pragma_state";
    if (candidate.reason.empty() && !baseline_problem.empty())
      candidate.reason = "baseline_invalid";
    if (candidate.reason.empty()) {
      auto trial = accepted;
      trial.push_back(&candidate);
      SourceOverrides overrides;
      if (!apply_edits(env, trial, overrides))
        candidate.reason = "edit_conflict";
      else {
        const auto artifact = render(overrides);
        for (unsigned i = 0; i < commands.size(); ++i) {
          const auto tentative = parse(commands[i], env, &artifact);
          std::string comparison_detail;
          candidate.reason =
              compare(baselines[i], tentative, &comparison_detail);
          if (!candidate.reason.empty()) {
            candidate.diagnostics = comparison_detail + tentative.diagnostics;
            break;
          }
        }
        if (candidate.reason.empty()) {
          accepted = std::move(trial);
          output = artifact;
          if (candidate.method)
            ++accepted_methods;
          else
            ++accepted_free;
          if (candidate.already_inline)
            ++already_inline;
          const auto &text = env.input.at(candidate.definition.where.file);
          removed_lines +=
              static_cast<unsigned>(std::count(
                  text.begin() + candidate.definition.where.begin,
                  text.begin() + candidate.definition.where.end, '\n')) +
              1;
        }
      }
    }
    const auto reason =
        candidate.reason.empty() ? "accepted" : candidate.reason;
    report["candidates"].push_back(
        {{"name", candidate.name},
         {"identity", key},
         {"file", candidate.file},
         {"line", candidate.line},
         {"kind", candidate.method ? "method" : "free"},
         {"reason", reason},
         {"reasonCode", reason.substr(0, reason.find(':'))},
         {"diagnostics", candidate.diagnostics},
         {"alreadyInline", candidate.already_inline},
         {"matched", candidate.matched}});
    std::cerr << "single_header: stitching " << candidate.file << ':'
              << candidate.line << ' ' << candidate.name << ": " << reason
              << '\n';
  }
  // Check the exact final accepted batch once more, including residual bodies.
  if (!accepted.empty()) {
    for (unsigned i = 0; i < commands.size(); ++i) {
      const auto final_result = parse(commands[i], env, &output);
      if (!compare(baselines[i], final_result).empty())
        throw std::runtime_error(
            "final stitching validation failed; no output was written");
    }
  }
  for (const auto &[path, text] : env.input) {
    if (read_bytes(path) != text)
      throw std::runtime_error("stitching input changed during analysis: " +
                               path);
  }
  report["counts"] = {{"candidates", candidates.size()},
                      {"matched", matched},
                      {"acceptedFree", accepted_free},
                      {"acceptedMethods", accepted_methods},
                      {"removedBodies", accepted.size()},
                      {"removedLines", removed_lines},
                      {"alreadyInline", already_inline},
                      {"skipped", candidates.size() - accepted.size()}};
  report["analysisMilliseconds"] =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started)
          .count();
  report["bytes"] = {
      {"unstitched", legacy.size()}, {"stitched", output.size()},
      {"savings", static_cast<std::int64_t>(legacy.size()) -
                      static_cast<std::int64_t>(output.size())}};
  report["retainsImplementation"] =
      accepted.size() != candidates.size() ||
      std::any_of(
          originals.begin(), originals.end(),
          [](const ParseResult &original) { return original.source_state; });
  report["skipReasons"] = json::object();
  for (const auto &[key, candidate] : candidates) {
    (void)key;
    if (candidate.reason.empty())
      continue;
    const auto code = candidate.reason.substr(0, candidate.reason.find(':'));
    const auto old = report["skipReasons"].value(code, 0u);
    report["skipReasons"][code] = old + 1;
  }
  if (!options.stitching_report.empty())
    write_text(options.stitching_report, report.dump(2) + '\n');
  std::cerr << "single_header: stitching accepted " << accepted.size() << '/'
            << candidates.size() << " definitions (free " << accepted_free
            << ", methods " << accepted_methods << ")\n";
  return output;
}

} // namespace single_header
