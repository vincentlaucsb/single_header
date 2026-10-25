#include "clang_sdk_allocator.hpp"
#include "frontend.hpp"
#include "source.hpp"
#include "validation.hpp"
#include "../helpers.hpp"

#include <clang/Tooling/JSONCompilationDatabase.h>
#include <llvm/Support/Signals.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;
using namespace clang;
using json = nlohmann::json;

namespace single_header {
using namespace stitching;

std::string stitch_functions(const Options &options,
                             const std::vector<fs::path> &headers,
                             const std::vector<fs::path> &sources,
                             const RenderArtifact &render) {
  const auto started = std::chrono::steady_clock::now();
  auto phase_started = started;
  json phases = json::object();
  auto finish_phase = [&](const char *name) {
    const auto now = std::chrono::steady_clock::now();
    phases[name] = std::chrono::duration_cast<std::chrono::milliseconds>(now - phase_started).count();
    phase_started = now;
  };
  std::size_t type_hits = 0, type_entries = 0;
  auto account = [&](const ParseResult &result) {
    type_hits += result.type_cache_hits;
    type_entries += result.type_cache_entries;
  };
  llvm::sys::PrintStackTraceOnErrorSignal("single_header");
  Environment env;
  env.inline_functions = options.inline_functions;
  env.require_cpp17 = options.function_stitching != "off";
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
  for (const auto &[path, text] : env.input)
    env.lexical.emplace(path, inspect_source(text));
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
  auto context_files = sources;
  if (context_files.empty() && options.inline_functions)
    context_files = headers;
  for (const auto &source : context_files) {
    if (database) {
      auto matches = database->getCompileCommands(source.string());
      if (matches.empty() || (!options.inline_functions && matches.size() != 1))
        throw std::runtime_error("expected exactly one compile command for " +
                                 source.string() + "; found " +
                                 std::to_string(matches.size()));
      for (auto &command : matches) {
        command.Filename = normalize_path(source).string();
        commands.push_back(std::move(command));
      }
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
                 {"inlineFunctions", options.inline_functions},
                 {"configurations", json::array()},
                 {"candidates", json::array()},
                 {"baseline", "valid"}};
  report["baselineDifferences"] = json::array();
  std::set<std::string> baseline_difference_keys;
  std::vector<ParseResult> originals, baselines;
  std::map<std::string, Candidate> candidates;
  // Run collation audits before any compiler work, including in inline mode.
  std::string legacy = render({});
  std::string baseline_problem, baseline_diagnostics;
  bool pragma = false;
  for (const auto &[path, facts] : env.lexical) {
    (void)path;
    for (const auto &command : facts.directives) {
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
  finish_phase("setup");
  std::cerr << "single_header: stitching discovering " << commands.size() << " source contexts\n";
  for (const auto &command : commands) {
    report["configurations"].push_back({{"source", command.Filename},
                                        {"directory", command.Directory},
                                        {"arguments", command.CommandLine}});
    auto original = parse(command, env, nullptr);
    account(original);
    if (original.errors)
      throw std::runtime_error("original analysis failed for " +
                               command.Filename + ":\n" + original.diagnostics);
    if (!original.inline_problem.empty())
      throw std::runtime_error(original.inline_problem);
    for (const auto &edit : original.inline_edits) {
      const auto duplicate = std::find_if(env.inline_edits.begin(), env.inline_edits.end(),
          [&](const Edit &existing) { return existing.where.file == edit.where.file &&
              existing.where.begin == edit.where.begin; });
      if (duplicate == env.inline_edits.end())
        env.inline_edits.push_back(edit);
    }
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
  if (options.inline_functions) {
    std::map<std::string, std::set<unsigned>> observed;
    std::map<std::string, std::vector<Span>> ranges;
    std::map<std::string, std::vector<Span>> definitions;
    for (const auto &original : originals) {
      for (const auto &[file, offsets] : original.observed_tokens)
        observed[file].insert(offsets.begin(), offsets.end());
      for (const auto &range : original.observed_ranges)
        ranges[range.file].push_back(range);
      for (const auto &range : original.definition_ranges) {
        ranges[range.file].push_back(range);
        definitions[range.file].push_back(range);
      }
    }
    for (const auto &original : originals) {
      for (const auto &macro : original.observed_ranges) {
        if (!env.sources.count(macro.file))
          continue;
        bool inside_function = false;
        for (const auto &definition : definitions[macro.file])
          if (definition.begin <= macro.begin && macro.end <= definition.end) {
            inside_function = true;
            break;
          }
        if (!inside_function) {
          const auto &text = env.input.at(macro.file);
          const auto line = 1 + std::count(text.begin(), text.begin() + macro.begin, '\n');
          throw std::runtime_error("--inline-functions cannot verify a source macro outside an "
              "observed function definition at " + macro.file + ':' +
              std::to_string(line) + "; declaration-producing macros require ordinary "
              "physical function definitions");
        }
      }
    }
    for (auto &[file, spans] : ranges) {
      (void)file;
      std::sort(spans.begin(), spans.end(), [](const Span &a, const Span &b) {
        return a.begin < b.begin;
      });
      std::vector<Span> merged;
      for (const auto &range : spans) {
        if (!merged.empty() && range.begin <= merged.back().end)
          merged.back().end = std::max(merged.back().end, range.end);
        else
          merged.push_back(range);
      }
      spans = std::move(merged);
    }
    // Every potentially emitted source declaration must be seen in a supplied context.
    // Whole observed bodies cover their inactive alternatives: adding inline to
    // the enclosing signature already protects those alternatives as well.
    for (const auto &[file, facts] : env.lexical) {
      if (!env.sources.count(file))
        continue;
      const auto &spans = ranges[file];
      std::size_t directive = 0, range = 0;
      for (const auto &token : facts.tokens) {
        while (directive < facts.directives.size() && facts.directives[directive].end <= token.offset)
          ++directive;
        if (directive < facts.directives.size() && facts.directives[directive].begin <= token.offset)
          continue;
        while (range < spans.size() && spans[range].end <= token.offset)
          ++range;
        if ((range < spans.size() && spans[range].begin <= token.offset) ||
            observed[file].count(token.offset))
          continue;
        const auto &text = env.input.at(file);
        const auto line = 1 + std::count(text.begin(), text.begin() + token.offset, '\n');
        throw std::runtime_error("--inline-functions has unobserved C++ tokens at " + file + ':' +
            std::to_string(line) + "; supply compilation contexts covering every emitted declaration "
            "(including conditional source branches)");
      }
    }
  }
  SourceOverrides inline_overrides;
  if (!apply_edits(env, {}, inline_overrides))
    throw std::runtime_error("function inline insertion conflicts; no output was written");
  if (options.inline_functions)
    legacy = render(inline_overrides);
  finish_phase("discovery");
  std::cerr << "single_header: stitching recording generated baselines\n";
  for (unsigned i = 0; i < commands.size(); ++i) {
    const auto &command = commands[i];
    const auto &original = originals[i];
    auto baseline = parse(command, env, &legacy);
    account(baseline);
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
  finish_phase("baseline");
  if (!baseline_problem.empty()) {
    if (options.inline_functions)
      throw std::runtime_error("inline function artifact validation failed; no output was written:\n" +
                               baseline_diagnostics);
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
  auto support_problem = [](const Candidate &candidate, const ParseResult &parsed) {
    const auto body = parsed.bodies.find(candidate.id);
    if (body == parsed.bodies.end())
      return std::string{}; // The declaration may be inactive in this context.
    if (!body->second.unsupported.empty())
      return body->second.unsupported;
    return body->second.internal ? std::string("internal_dependency") : std::string{};
  };
  for (auto &[key, candidate] : candidates) {
    if (candidate.matched)
      ++matched;
    if (candidate.reason.empty() &&
        (options.function_stitching == "off" ||
         (options.function_stitching != "all" &&
          candidate.method != (options.function_stitching == "methods"))))
      candidate.reason = "mode_excluded";
    if (candidate.reason.empty() && pragma)
      candidate.reason = "unsupported_pragma_state";
    if (candidate.reason.empty() && !baseline_problem.empty())
      candidate.reason = "baseline_invalid";
    if (candidate.reason.empty())
      for (const auto &baseline : baselines) {
        candidate.reason = support_problem(candidate, baseline);
        if (!candidate.reason.empty())
          break;
      }
  }
  struct TrialResult {
    std::string artifact, reason, diagnostics;
    std::set<std::string> suspects;
  };
  // Run-scoped source of truth: a key contains the complete ordered edit set.
  // Frozen inputs, rendering options and frontend contexts never change here.
  std::map<std::vector<std::string>, TrialResult> trials;
  unsigned trial_parses = 0, trial_attempts = 0, cache_hits = 0;
  auto validate = [&](const std::vector<const Candidate *> &trial)
      -> const TrialResult & {
    std::vector<std::string> key;
    for (const auto *candidate : trial)
      key.push_back(candidate->id);
    std::sort(key.begin(), key.end());
    ++trial_attempts;
    const auto found = trials.find(key);
    if (found != trials.end()) {
      ++cache_hits;
      return found->second;
    }
    TrialResult result;
    SourceOverrides overrides;
    if (!apply_edits(env, trial, overrides))
      result.reason = "edit_conflict";
    else {
      result.artifact = render(overrides);
      for (unsigned i = 0; i < commands.size(); ++i) {
        ++trial_parses;
        const auto tentative = parse(commands[i], env, &result.artifact);
        account(tentative);
        std::string detail;
        for (const auto *candidate : trial) {
          result.reason = support_problem(*candidate, tentative);
          if (!result.reason.empty()) {
            detail = "unsupported relocated function: " + candidate->name + '\n';
            result.suspects.insert(candidate->id);
            break;
          }
        }
        if (result.reason.empty())
          result.reason = compare(baselines[i], tentative, &detail);
        if (!result.reason.empty()) {
          result.diagnostics = detail + tentative.diagnostics;
          for (const auto *candidate : trial) {
            // Attribution is a partitioning hint, never acceptance evidence.
            // A unique exact emitted replacement provides a bounded range even
            // after collation; changed/ambiguous text falls back to subdivision.
            auto replacement = replace_all(candidate->declaration.replacement, "\r\n", "\n");
            const auto begin = result.artifact.find(replacement);
            if (!replacement.empty() && begin != std::string::npos &&
                result.artifact.find(replacement, begin + 1) == std::string::npos)
              for (const auto offset : tentative.artifact_error_offsets)
                if (offset >= begin && offset < begin + replacement.size())
                  result.suspects.insert(candidate->id);
            const auto &id = candidate->id;
            if ((result.reason.find("changed_semantics:") == 0 ||
                 result.reason.find("changed_declaration:") == 0 ||
                 result.reason.find("missing_definition:") == 0) &&
                result.reason.size() >= id.size() &&
                result.reason.compare(result.reason.size() - id.size(), id.size(), id) == 0)
              result.suspects.insert(id);
          }
          break;
        }
      }
    }
    return trials.emplace(std::move(key), std::move(result)).first->second;
  };
  std::vector<Candidate *> eligible;
  for (auto &[key, candidate] : candidates) {
    (void)key;
    if (candidate.reason.empty())
      eligible.push_back(&candidate);
  }
  unsigned guided_partitions = 0;
  std::function<void(const std::vector<Candidate *> &)> accept_batch =
      [&](const std::vector<Candidate *> &group) {
    if (group.empty())
      return;
    auto trial = accepted;
    trial.insert(trial.end(), group.begin(), group.end());
    const auto &result = validate(trial);
    if (result.reason.empty()) {
      accepted = std::move(trial);
      output = result.artifact;
      return;
    }
    if (group.size() == 1) {
      group.front()->reason = result.reason;
      group.front()->diagnostics = result.diagnostics;
      return;
    }
    std::vector<Candidate *> rest, suspects;
    for (auto *candidate : group)
      (result.suspects.count(candidate->id) ? suspects : rest).push_back(candidate);
    if (!suspects.empty() && !rest.empty()) {
      ++guided_partitions;
      accept_batch(rest);
      accept_batch(suspects);
      return;
    }
    const auto middle = group.begin() + group.size() / 2;
    accept_batch(std::vector<Candidate *>(group.begin(), middle));
    accept_batch(std::vector<Candidate *>(middle, group.end()));
  };
  std::cerr << "single_header: stitching validating " << eligible.size() << " eligible edits in batches\n";
  accept_batch(eligible);
  finish_phase("trials");
  for (const auto *candidate : accepted) {
    if (candidate->method)
      ++accepted_methods;
    else
      ++accepted_free;
    if (candidate->already_inline)
      ++already_inline;
    const auto &text = env.input.at(candidate->definition.where.file);
    removed_lines += static_cast<unsigned>(std::count(
        text.begin() + candidate->definition.where.begin,
        text.begin() + candidate->definition.where.end, '\n')) + 1;
  }
  for (const auto &[key, candidate] : candidates) {
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
  if (!accepted.empty() || options.inline_functions) {
    std::cerr << "single_header: stitching freshly validating final artifact\n";
    for (unsigned i = 0; i < commands.size(); ++i) {
      const auto final_result = parse(commands[i], env, &output);
      account(final_result);
      for (const auto *candidate : accepted)
        if (!support_problem(*candidate, final_result).empty())
          throw std::runtime_error("final stitching support validation failed; no output was written");
      if (!compare(baselines[i], final_result).empty())
        throw std::runtime_error(
            "final stitching validation failed; no output was written");
    }
  }
  finish_phase("final");
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
                      {"inlineInsertions", env.inline_edits.size()},
                      {"skipped", candidates.size() - accepted.size()}};
  report["work"] = {
      {"originalParses", commands.size()}, {"baselineParses", commands.size()},
      {"trialParses", trial_parses}, {"trialAttempts", trial_attempts},
      {"trialCacheHits", cache_hits},
      {"guidedPartitions", guided_partitions},
      {"finalParses", accepted.empty() && !options.inline_functions ? 0 : commands.size()},
      {"lexedFiles", env.lexical.size()},
      {"typeCacheHits", type_hits}, {"typeCacheEntries", type_entries}};
  report["phaseMilliseconds"] = phases;
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
