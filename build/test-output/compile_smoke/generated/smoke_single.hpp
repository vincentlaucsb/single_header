#pragma once

namespace smoke {
struct Thing {
  int value() const { return 40; }
};

inline int answer() { return 1; }
}


namespace smoke {
int generated();
}


namespace smoke {
int generated() { return 1; }
}


