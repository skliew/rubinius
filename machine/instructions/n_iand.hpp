#include "instructions.hpp"

namespace rubinius {
  namespace instructions {
    inline void n_iand(CF, R0, R1, R2) {
      REG(r0) = REG(r1) & REG(r2);
    }
  }
}
