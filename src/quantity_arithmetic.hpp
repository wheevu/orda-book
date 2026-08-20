#pragma once

#include "types.hpp"

#include <limits>

namespace lob {

inline bool quantity_add_would_overflow(Quantity left, Quantity right) {
  return right > 0 && left > std::numeric_limits<Quantity>::max() - right;
}

}  // namespace lob
