#include "action_chain.h"

namespace romkatv {

thread_local ActionChain::Mem ActionChain::mem_;

void ActionChain::Work::RunAllSlow(Work* w, Work* next) {
  do {
    assert(w != nullptr && w != Sealed());
    assert(next != nullptr && next != Sealed());
    w->Destroy();
    ::operator delete(w, 64);
    w = next;
    w->invoke_(w);
    next = w->next_.exchange(Sealed(), std::memory_order_acq_rel);
  } while (next);
}

}  // namespace romkatv
