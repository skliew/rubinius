#include "instructions/n_dle.hpp"

namespace rubinius {
  namespace interpreter {
    intptr_t n_dle(STATE, CallFrame* call_frame, intptr_t const opcodes[]) {
      instructions::n_dle(call_frame, argument(0), argument(1), argument(2));

      call_frame->next_ip(instructions::data_n_dle.width);

      return ((instructions::Instruction)opcodes[call_frame->ip()])(state, call_frame, opcodes);
    }
  }
}
