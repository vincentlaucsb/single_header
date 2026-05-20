#pragma once
#include <cstddef>

#define CSV_INLINE inline

namespace csv {
using size_type = std::size_t;
}


namespace csv {
CSV_INLINE size_type row_size() { return 3; }
int parsed_rows();
}


namespace csv {
CSV_INLINE int version() { return 1; }
}

#include <vector>

namespace csv {
int parsed_rows() {
  std::vector<int> rows{1, 2, 3};
  return static_cast<int>(rows.size());
}
}


