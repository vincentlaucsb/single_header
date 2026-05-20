#include "row.hpp"
#include <vector>

namespace csv {
int parsed_rows() {
  std::vector<int> rows{1, 2, 3};
  return static_cast<int>(rows.size());
}
}
