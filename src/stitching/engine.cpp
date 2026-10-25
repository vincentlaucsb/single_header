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
