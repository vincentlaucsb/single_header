#pragma once
#include <string>
#include <string_view>

namespace single_header {
// Optional text-level cleanup; see docs/namespace-merging.md for macro limits.
std::string merge_adjacent_namespaces(std::string_view source);
} // namespace single_header
