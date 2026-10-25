#pragma once

#include "model.hpp"

namespace single_header::stitching {
std::string compare(const ParseResult &before, const ParseResult &after,
                    std::string *detail = nullptr, bool exact = true);
} // namespace single_header::stitching
