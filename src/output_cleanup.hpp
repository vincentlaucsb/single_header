#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace single_header {
// Final renderer cleanup shared by the lightweight and compiler-backed paths.
std::string cleanup_output(std::string_view source, bool deduplicate_includes = false,
                           std::string_view include_guard = {},
                           const std::vector<std::string> &allow_mixed_includes = {},
                           bool merge_namespaces = false);
} // namespace single_header
