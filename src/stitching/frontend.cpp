#include "clang_sdk_allocator.hpp"
#include "frontend.hpp"
#include "semantics.hpp"
#include "source.hpp"
#include "../helpers.hpp"

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
#include <clang/Lex/Lexer.h>
#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/raw_ostream.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <memory>
#include <utility>

using namespace clang;
using json = nlohmann::json;

namespace single_header::stitching {
namespace {
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
    return declaration_identity(decl, ctx, env.identities);
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
    Record record = function_semantics(fn, ctx, env.identities);
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
    result.declarations["variable:" + id(variable)] =
        variable_initializer(variable, ctx, env.identities);
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
    result.declarations["field:" + id(field)] =
        field_initializer(field, ctx, env.identities);
    return true;
  }
  bool VisitFunctionDecl(FunctionDecl *fn) {
    if (!project(fn->getLocation()) || fn->isImplicit())
      return true;
    if (const auto *method = dyn_cast<CXXMethodDecl>(fn))
      if (method->getParent()->isLambda())
        return true; // Included in the enclosing body's record.
    const auto key = id(fn);
    result.declarations["type:" + key] =
        function_type(fn, ctx, env.identities);
    std::string attributes;
    llvm::raw_string_ostream attribute_output(attributes);
    for (const auto *attribute : fn->attrs())
      attribute->printPretty(attribute_output,
                             PrintingPolicy(ctx.getLangOpts()));
    result.declarations["attributes:" + key] = attributes;
    for (unsigned i = 0; i < fn->getNumParams(); ++i) {
      if (fn->getParamDecl(i)->hasDefaultArg() &&
          !fn->getParamDecl(i)->hasUnparsedDefaultArg()) {
        result.declarations["default:" + key + ':' + std::to_string(i)] =
            default_argument(fn, i, ctx, env.identities);
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
} // namespace

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

} // namespace single_header::stitching
