#include "clang_sdk_allocator.hpp"
#include "frontend.hpp"
#include "inline_functions.hpp"
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
#include <clang/Lex/MacroArgs.h>
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
// One canonical identity per SourceManager FileID, shared by preprocessing and AST discovery.
struct FileIndex {
  struct Entry { std::string name; bool project = false; };
  const Environment &env;
  std::map<unsigned, Entry> entries;
  std::map<unsigned, FileFacts> generated_lexical;
  Entry empty;
  explicit FileIndex(const Environment &environment) : env(environment) {}
  const Entry &get(SourceLocation location, const SourceManager &sm) {
    if (location.isInvalid())
      return empty;
    location = sm.getExpansionLoc(location);
    const auto id = sm.getFileID(location).getHashValue();
    const auto found = entries.find(id);
    if (found != entries.end())
      return found->second;
    Entry entry;
    entry.name = stitching::file_name(location, sm);
    entry.project = entry.name == env.artifact || env.headers.count(entry.name) || env.sources.count(entry.name);
    return entries.emplace(id, std::move(entry)).first->second;
  }
  bool inside_directive(SourceLocation location, const SourceManager &sm) {
    location = sm.getExpansionLoc(location);
    const auto &entry = get(location, sm);
    const auto known = env.lexical.find(entry.name);
    const FileFacts *facts = nullptr;
    if (known != env.lexical.end())
      facts = &known->second;
    else {
      const auto id = sm.getFileID(location);
      auto found = generated_lexical.find(id.getHashValue());
      if (found == generated_lexical.end())
        found = generated_lexical.emplace(id.getHashValue(), inspect_source(sm.getBufferData(id).str())).first;
      facts = &found->second;
    }
    const auto offset = sm.getFileOffset(location);
    const auto &directives = facts->directives;
    const auto next = std::upper_bound(directives.begin(), directives.end(), offset,
        [](unsigned value, const Directive &directive) { return value < directive.begin; });
    return next != directives.begin() && offset < std::prev(next)->end;
  }
};
const std::string &event_file(const TokenInfo &token) { return token.file; }
const std::string &event_file(const Expansion &macro) { return macro.where.file; }
unsigned event_offset(const TokenInfo &token) { return token.offset; }
unsigned event_offset(const Expansion &macro) { return macro.where.begin; }
unsigned event_end(const TokenInfo &token) { return token.offset + 1; }
unsigned event_end(const Expansion &macro) { return macro.where.end; }

// Map the entire invocation (or argument), not merely the expansion's first
// token, to the ultimate physical source. Nested/re-expanded invocations can
// share their outer invocation's span; those bytes must all move together.
Span expansion_span(SourceRange range, const SourceManager &sm,
                    const LangOptions &language, FileIndex &files) {
  if (range.isInvalid()) return {};
  const auto physical = Lexer::makeFileCharRange(sm.getExpansionRange(range), sm, language);
  if (physical.isInvalid() || physical.getBegin().isMacroID() || physical.getEnd().isMacroID() ||
      sm.getFileID(physical.getBegin()) != sm.getFileID(physical.getEnd())) return {};
  return {files.get(physical.getBegin(), sm).name, sm.getFileOffset(physical.getBegin()),
          sm.getFileOffset(physical.getEnd())};
}
bool contains(const Span &outer, const Span &inner) {
  return outer.valid() && inner.valid() && outer.file == inner.file &&
         outer.begin <= inner.begin && inner.end <= outer.end;
}

// Offset indexes bound each query. Restoring pointer/encounter order preserves
// fingerprints even when repeated includes revisit earlier physical offsets.
template<class Event> struct EventIndex {
  std::map<std::string, std::vector<const Event *>> files;
  std::map<std::string, std::vector<unsigned>> maximum_ends;
  explicit EventIndex(const std::vector<Event> &events) {
    for (const auto &event : events)
      files[event_file(event)].push_back(&event);
    for (auto &[file, entries] : files) {
      (void)file;
      std::stable_sort(entries.begin(), entries.end(), [](const Event *a, const Event *b) {
        return event_offset(*a) < event_offset(*b);
      });
      auto &ends = maximum_ends[file];
      unsigned maximum = 0;
      for (const auto *entry : entries) {
        maximum = std::max(maximum, event_end(*entry));
        ends.push_back(maximum);
      }
    }
  }
  std::vector<const Event *> range(const std::string &file, unsigned begin = 0,
                                   unsigned end = ~0u) const {
    const auto found = files.find(file);
    if (found == files.end())
      return {};
    const auto &entries = found->second;
    auto lower = [&](unsigned offset) {
      return std::lower_bound(entries.begin(), entries.end(), offset,
          [](const Event *event, unsigned value) { return event_offset(*event) < value; });
    };
    std::vector<const Event *> result(lower(begin), lower(end));
    std::sort(result.begin(), result.end(), std::less<const Event *>{});
    return result;
  }
  std::vector<const Event *> overlapping(const std::string &file, unsigned begin,
                                         unsigned end) const {
    const auto found = files.find(file);
    if (found == files.end()) return {};
    const auto &entries = found->second;
    const auto stop = std::lower_bound(entries.begin(), entries.end(), end,
        [](const Event *event, unsigned value) { return event_offset(*event) < value; });
    const auto &ends = maximum_ends.at(file);
    const auto first = std::upper_bound(ends.begin(), ends.begin() + (stop - entries.begin()), begin);
    std::vector<const Event *> result;
    for (auto entry = entries.begin() + (first - ends.begin()); entry != stop; ++entry)
      if (event_end(**entry) > begin) result.push_back(*entry);
    std::sort(result.begin(), result.end(), std::less<const Event *>{});
    return result;
  }

};
bool supported_attributes(const FunctionDecl *fn, bool definition) {
  for (const auto *attribute : fn->attrs())
    if (!isa<WarnUnusedResultAttr>(attribute) ||
        (definition && !attribute->isInherited()))
      return false;
  return true;
}
struct Collector : RecursiveASTVisitor<Collector> {
  ASTContext &ctx;
  ParseResult &result;
  const Environment &env;
  FileIndex &files;
  const EventIndex<TokenInfo> &tokens;
  const EventIndex<Expansion> &expansions;
  SemanticCache semantic_cache;
  bool discover;
  Collector(ASTContext &context, ParseResult &output,
            const Environment &environment,
            FileIndex &file_index, const EventIndex<TokenInfo> &input_tokens,
            const EventIndex<Expansion> &macro_expansions, bool discovery)
      : ctx(context), result(output), env(environment), files(file_index), tokens(input_tokens),
        expansions(macro_expansions), discover(discovery) {}

  std::string file_name(SourceLocation location, const SourceManager &sm) const {
    return files.get(location, sm).name;
  }
  Span span(SourceRange range, const SourceManager &sm, const LangOptions &lang) const {
    const auto &file = files.get(range.getBegin(), sm).name;
    return stitching::span(range, sm, lang, &file);
  }
  bool project(SourceLocation loc) const {
    return files.get(loc, ctx.getSourceManager()).project;
  }
  std::string id(const Decl *decl) {
    return declaration_identity(decl, ctx, env.identities, semantic_cache);
  }
  Span definition_span(FunctionDecl *fn) const {
    auto range = fn->getSourceRange();
    if (range.getBegin().isMacroID()) {
      const auto &sm = ctx.getSourceManager();
      const auto file = file_name(range.getBegin(), sm);
      const auto offset =
          sm.getFileOffset(sm.getExpansionLoc(range.getBegin()));
      bool allowed = false;
      for (const auto *expansion : expansions.range(file, offset, offset + 1))
        if (expansion->annotation)
          allowed = true;
      if (!allowed)
        return {};
      range.setBegin(sm.getExpansionLoc(range.getBegin()));
    }
    return span(range, ctx.getSourceManager(), ctx.getLangOpts());
  }
  Record record(FunctionDecl *fn) {
    Record record = function_semantics(fn, ctx, env.identities, semantic_cache);
    auto range = span(fn->getBody()->getSourceRange(),
                       ctx.getSourceManager(), ctx.getLangOpts());
    if (const auto *constructor = dyn_cast<CXXConstructorDecl>(fn))
      for (const auto *init : constructor->inits())
        if (init->isWritten()) {
          const auto location = ctx.getSourceManager().getExpansionLoc(init->getSourceLocation());
          if (file_name(location, ctx.getSourceManager()) == range.file)
            range.begin = std::min(range.begin, ctx.getSourceManager().getFileOffset(location));
        }
    for (const auto *event : tokens.range(range.file, range.begin, range.end)) {
      const auto &token = *event;
      if (token.file == range.file && token.offset >= range.begin &&
          token.offset < range.end) {
        record.tokens +=
            std::to_string(token.spelling.size()) + ':' + token.spelling + ';';
      }
    }
    Span interior;
    if (const auto *compound = dyn_cast<CompoundStmt>(fn->getBody())) {
      const auto &sm = ctx.getSourceManager();
      if (compound->getLBracLoc().isValid() && compound->getRBracLoc().isValid() &&
          compound->getLBracLoc().isFileID() && compound->getRBracLoc().isFileID()) {
        const auto first = Lexer::getLocForEndOfToken(compound->getLBracLoc(), 0, sm, ctx.getLangOpts());
        if (first.isValid() && sm.getFileID(first) == sm.getFileID(compound->getRBracLoc()))
          interior = {range.file, sm.getFileOffset(first), sm.getFileOffset(compound->getRBracLoc())};
      }
    }
    for (const auto *event : expansions.overlapping(range.file, range.begin, range.end)) {
      const auto &expansion = *event;
      bool safe = expansion.body_safe && expansion.complete && contains(range, expansion.where);
      if (expansion.function_like) {
        safe = safe && contains(interior, expansion.where);
        for (const auto &argument : expansion.arguments)
          safe = safe && contains(expansion.where, argument) && contains(interior, argument);
      }
      if (!safe && record.unsupported.empty()) record.unsupported = "body_macro";
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
        type_name(variable->getType(), ctx, semantic_cache);
    if (!variable->hasInit())
      return true;
    result.declarations["variable:" + id(variable)] =
        variable_initializer(variable, ctx, env.identities, semantic_cache);
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
      value += ':' + type_name(field->getType(), ctx, semantic_cache) + ':' +
               std::to_string(layout.getFieldOffset(field_index++));
    }
    result.declarations["layout:" + id(record)] = std::move(value);
    return true;
  }
  bool VisitFieldDecl(FieldDecl *field) {
    if (!field->hasInClassInitializer() || !project(field->getLocation()))
      return true;
    result.declarations["field:" + id(field)] =
        field_initializer(field, ctx, env.identities, semantic_cache);
    return true;
  }
  bool VisitFunctionTemplateDecl(FunctionTemplateDecl *decl) {
    if (env.inline_functions)
      for (auto *fn : decl->specializations()) collect_inline_function(fn, ctx, env, result);
    return true;
  }
  bool VisitClassTemplateSpecializationDecl(ClassTemplateSpecializationDecl *decl) {
    if (env.inline_functions) collect_inline_class_instantiation(decl, ctx, env, result);
    return true;
  }
  bool VisitFunctionDecl(FunctionDecl *fn) {
    if (env.inline_functions) collect_inline_function(fn, ctx, env, result);
    if (!project(fn->getLocation()) || fn->isImplicit())
      return true;
    if (const auto *method = dyn_cast<CXXMethodDecl>(fn))
      if (method->getParent()->isLambda())
        return true; // Included in the enclosing body's record.
    const auto key = id(fn);
    result.declarations["type:" + key] =
        function_type(fn, ctx, env.identities, semantic_cache);
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
            default_argument(fn, i, ctx, env.identities, semantic_cache);
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
    if (fn->isDefaulted() || fn->isDeleted())
      return "defaulted_or_deleted_function";
    if (!fn->getBody())
      return "unparsed_function_body";
    if (isa<CXXConstructorDecl>(fn) && !isa<CompoundStmt>(fn->getBody()))
      return "constructor_try_block";
    if (!isa<CXXMethodDecl>(fn) && fn->isOverloadedOperator()) {
      const auto op = fn->getOverloadedOperator();
      if (op == OO_New || op == OO_Array_New || op == OO_Delete || op == OO_Array_Delete)
        return "global_allocation_replacement";
    }
    if (fn->isConstexpr() || fn->isConsteval() || fn->isVariadic() ||
        fn->isExternC())
      return "unsupported_function";
    if (fn->getLinkageInternal() != Linkage::External)
      return "internal_linkage";
    if (!supported_attributes(fn, true))
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
    const auto &facts = env.lexical.at(range.file);
    const auto &physical_tokens = facts.tokens;
    const auto first_directive = std::lower_bound(facts.directives.begin(), facts.directives.end(), range.begin,
        [](const Directive &directive, unsigned value) { return directive.begin < value; });
    for (auto entry = first_directive; entry != facts.directives.end() && entry->begin < range.end; ++entry) {
      const auto &directive = *entry;
      if (!directive.words.empty()) {
        const auto &kind = directive.words.front();
        if (kind != "if" && kind != "ifdef" && kind != "ifndef" &&
            kind != "else" && kind != "elif" && kind != "endif")
          return "body_directive";
      }
    }
    auto token = std::lower_bound(physical_tokens.begin(), physical_tokens.end(), range.begin,
        [](const PhysicalToken &entry, unsigned value) { return entry.offset < value; });
    for (; token != physical_tokens.end() && token->offset < range.end; ++token) {
      if (token->text == "_Pragma" || token->text == "__pragma")
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
    if (!supported_attributes(declaration, false)) {
      candidate.reason = "unsupported_attribute";
      return;
    }
    const auto &header = env.input.at(destination.file);
    for (const auto *event : expansions.range(destination.file, 0, destination.begin + 1)) {
      const auto &macro = *event;
      if (macro.where.file == destination.file &&
          macro.where.end <= destination.begin &&
          !has_tokens_between(env.lexical.at(destination.file), macro.where.end, destination.begin)) {
        candidate.reason = "declaration_macro";
        return;
      }
    }
    const auto source_location = definition_span(fn);
    const auto &header_facts = env.lexical.at(destination.file);
    if (conditional_context(header_facts, destination.begin) !=
            conditional_context(env.lexical.at(source_location.file), source_location.begin) ||
        std::any_of(header_facts.directives.begin(), header_facts.directives.end(),
                    [&](const Directive &directive) {
                      return directive.begin >= destination.begin &&
                             directive.begin < destination.end;
                    })) {
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
    std::string initializers;
    if (const auto *constructor = dyn_cast<CXXConstructorDecl>(fn)) {
      if (std::any_of(constructor->init_begin(), constructor->init_end(),
                      [](const CXXCtorInitializer *init) { return init->isWritten(); })) {
        const auto *type_info = fn->getTypeSourceInfo();
        if (!type_info) {
          candidate.reason = "constructor_initializer_range";
          return;
        }
        const auto proto = type_info->getTypeLoc()
                               .getAs<FunctionProtoTypeLoc>();
        if (proto.isNull() || proto.getRParenLoc().isInvalid() || proto.getRParenLoc().isMacroID()) {
          candidate.reason = "constructor_initializer_range";
          return;
        }
        const auto after_parameters = sm.getFileOffset(proto.getRParenLoc()) + 1;
        unsigned depth = 0;
        for (const auto &token : env.lexical.at(source_range.file).tokens) {
          if (token.offset < after_parameters || token.offset >= body_range.begin)
            continue;
          if (token.text == "(" || token.text == "[")
            ++depth;
          if ((token.text == ")" || token.text == "]") && depth)
            --depth;
          if (token.text == ":" && depth == 0) {
            initializers = source.substr(token.offset, body_range.begin - token.offset);
            break;
          }
        }
        if (initializers.empty()) {
          candidate.reason = "constructor_initializer_range";
          return;
        }
      }
    }
    // Empty annotation expansions may precede AST begin locations. Include
    // their complete spelling range in the paired source removal.
    const auto preceding = expansions.range(source_range.file, 0, source_range.begin + 1);
    for (auto entry = preceding.rbegin(); entry != preceding.rend(); ++entry) {
      const auto *macro = *entry;
      if (macro->where.file != source_range.file)
        continue;
      if (macro->where.end <= source_range.begin &&
          !has_tokens_between(env.lexical.at(source_range.file), macro->where.end, source_range.begin)) {
        if (!macro->annotation) {
          candidate.reason = "signature_macro";
          return;
        }
        source_range.begin = macro->where.begin;
      }
    }
    auto signature_macros = expansions.range(source_range.file, source_range.begin, body_range.begin);
    const auto header_macros = expansions.range(destination.file, destination.begin, destination.end);
    signature_macros.insert(signature_macros.end(), header_macros.begin(), header_macros.end());
    for (const auto *event : signature_macros) {
      const auto &macro = *event;
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
            signature + " " + initializers + body};
    candidate.definition = {source_range, ""};
  }
};

struct Diagnostics : DiagnosticConsumer {
  ParseResult &result;
  const Environment &env;
  Diagnostics(ParseResult &output, const Environment &environment)
      : result(output), env(environment) {}
  void HandleDiagnostic(DiagnosticsEngine::Level level,
                        const Diagnostic &info) override {
    if (level >= DiagnosticsEngine::Error) {
      result.errors = true;
      if (info.hasSourceManager() && info.getLocation().isValid()) {
        const auto &sm = info.getSourceManager();
        const auto location = sm.getExpansionLoc(info.getLocation());
        if (stitching::file_name(location, sm) == env.artifact)
          result.artifact_error_offsets.push_back(sm.getFileOffset(location));
      }
    }
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
  FileIndex files;
  bool discover;
  Action(ParseResult &output, const Environment &environment, bool discovery)
      : result(output), env(environment), files(environment), discover(discovery) {}
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &ci,
                                                 llvm::StringRef) override {
    if (env.require_cpp17 && !ci.getLangOpts().CPlusPlus17) {
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
            action.files.get(location, ci.getSourceManager()).name == action.env.artifact)
          action.result.artifact_seen = true;
      }
      void MacroExpands(const Token &name, const MacroDefinition &definition,
                        SourceRange range, const MacroArgs *arguments) override {
        const auto &sm = ci.getSourceManager();
        const auto location = sm.getExpansionLoc(range.getBegin());
        const auto end = Lexer::getLocForEndOfToken(
            sm.getExpansionLoc(range.getEnd()), 0, sm, ci.getLangOpts());
        if (location.isInvalid() || end.isInvalid())
          return;
        const auto &identity = action.files.get(location, sm);
        if (!identity.project)
          return;
        const auto &physical_file = identity.name;
        if (action.files.inside_directive(location, sm))
          return; // An expansion inside #if is not a signature annotation.
        const auto spelling = ci.getPreprocessor().getSpelling(name);
        const auto *info = definition.getMacroInfo();
        const bool annotation = action.env.annotations.count(spelling) &&
                                info && !info->isFunctionLike() &&
                                (info->getNumTokens() == 0 ||
                                 (info->getNumTokens() == 1 &&
                                  ci.getPreprocessor().getSpelling(
                                      *info->tokens_begin()) == "inline"));
        bool body_safe = info && spelling != "_Pragma" && spelling != "__pragma";
        if (info)
          for (const auto &token : info->tokens()) {
            const auto word = ci.getPreprocessor().getSpelling(token);
            if (word == "_Pragma" || word == "__pragma" ||
                token.is(tok::hash) || token.is(tok::hashhash))
              body_safe = false;
          }
        const auto invocation = expansion_span(range, sm, ci.getLangOpts(), action.files);
        Expansion expansion{invocation.valid() ? invocation : Span{physical_file, sm.getFileOffset(location), sm.getFileOffset(end)},
                            spelling, annotation, body_safe, info && info->isFunctionLike(), invocation.valid(), {}};
        if (expansion.function_like && !arguments) expansion.complete = false;
        if (arguments) {
          for (unsigned i = 0; i < arguments->getNumMacroArguments(); ++i) {
            const auto *first = arguments->getUnexpArgument(i);
            const auto length = MacroArgs::getArgLength(first);
            if (!length) continue; // Empty/erased arguments still belong to the full invocation span.
            const auto argument = expansion_span(SourceRange(first->getLocation(), first[length - 1].getLocation()), sm, ci.getLangOpts(), action.files);
            expansion.complete = expansion.complete && contains(invocation, argument);
            expansion.arguments.push_back(argument);
          }
        }
        action.expansions.push_back(std::move(expansion));
      }
      void PragmaDirective(SourceLocation location, PragmaIntroducerKind) override {
        const auto &sm = ci.getSourceManager();
        const auto &identity = action.files.get(location, sm);
        if (!identity.project || location.isFileID()) return;
        const auto invocation = expansion_span(SourceRange(location), sm, ci.getLangOpts(), action.files);
        if (invocation.valid())
          action.expansions.push_back({invocation, "<macro pragma>", false, false, false, true, {}});

      }
    };
    ci.getPreprocessor().addPPCallbacks(std::make_unique<Macros>(*this, ci));
    ci.getPreprocessor().setTokenWatcher([this, &ci](const Token &token) {
      if (token.is(tok::eof) || token.getLocation().isInvalid())
        return;
      const auto &sm = ci.getSourceManager();
      const auto &identity = files.get(token.getLocation(), sm);
      const auto &file = identity.name;
      if (file == env.artifact)
        result.artifact_seen = true;
      if (!identity.project)
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
        if (action.env.inline_functions) {
          for (const auto &token : action.tokens)
            action.result.observed_tokens[token.file].insert(token.offset);
          for (const auto &expansion : action.expansions)
            if (expansion.where.valid()) action.result.observed_ranges.push_back(expansion.where);
        }
        const EventIndex<TokenInfo> tokens(action.tokens);
        const EventIndex<Expansion> expansions(action.expansions);
        Collector collector(ctx, action.result, action.env, action.files, tokens,
                            expansions, action.discover);
        collector.TraverseDecl(ctx.getTranslationUnitDecl());
        action.result.type_cache_hits = collector.semantic_cache.type_hits;
        action.result.type_cache_entries = collector.semantic_cache.types.size();
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
  Diagnostics diagnostics(result, env);
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
