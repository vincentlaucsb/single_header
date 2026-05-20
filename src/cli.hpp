#pragma once

#include "amalgamator.hpp"

#include <string>

namespace single_header {

std::string usage();
Options parse_args(int argc, char **argv);

} // namespace single_header
