#pragma once

#include "amalgamator.hpp"

#include <functional>
#include <map>

namespace single_header {

// Edits are applied to original files before the existing collation pipeline.
using SourceOverrides = std::map<std::string, std::string>;
using RenderArtifact = std::function<std::string(const SourceOverrides &)>;

std::string stitch_functions(const Options &options,
                             const std::vector<std::filesystem::path> &headers,
                             const std::vector<std::filesystem::path> &sources,
                             const RenderArtifact &render);
void validate_stitching_options(const Options &options);
bool paths_alias(const std::filesystem::path &left,
                 const std::filesystem::path &right);

} // namespace single_header
