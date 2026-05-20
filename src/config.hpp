#pragma once

#include "amalgamator.hpp"

#include <filesystem>

namespace single_header {

Options read_config(const std::filesystem::path &config_path);

} // namespace single_header
