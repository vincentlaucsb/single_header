#pragma once
#include <string>
#include "detail.hpp"

namespace simple {
std::string name();
inline int value() { return detail(); }
}

/** INSERT_CSV_SOURCES **/
