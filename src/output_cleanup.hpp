#pragma once

#include <string>
#include <string_view>

namespace single_header {
// Final renderer cleanup shared by the lightweight and compiler-backed paths.
std::string cleanup_output(std::string_view source);
} // namespace single_header
