#pragma once
#include <string>

namespace simple {
inline int detail() { return 7; }
}


namespace simple {
std::string name();
inline int value() { return detail(); }
}

#include <string>

namespace simple {
std::string name() { return "simple"; }
}


