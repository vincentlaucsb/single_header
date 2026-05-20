#pragma once

namespace smoke {
struct Thing {
  int value() const { return 40; }
};

inline int answer() { return 1; }
}
