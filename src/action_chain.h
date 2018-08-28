#ifndef ROMKATV_ACTION_CHAIN_ACTION_CHAIN_H_
#define ROMKATV_ACTION_CHAIN_ACTION_CHAIN_H

#include <stddef.h>

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>

namespace romkatv {

// Wait-free queue of actions. Can be used as an alternative to locking.
//
// TODO: Figure out whether memory order constraints can be relaxed.
class ActionChain {
 public:
  ActionChain() { tail_.load()->Run(); }
  ~ActionChain() { tail_.load()->Destroy(); }
  ActionChain(ActionChain&&) = delete;

  // Either executes `action` synchronously (in which case some other actions added
  // concurrently by other threads may also run synchronously after `f` returns)
  // or schedules it for execution after all previously scheduled actions have
  // completed.
  //
  // Actions are guaranteed to run in the same order they were added.
  template <class F>
  void Add(F&& action) {
    Work* work = Work::New(std::forward<F>(action));
    tail_.exchange(work, std::memory_order_acq_rel)->ContinueWith(work);
  }

 private:
  class Work {
   public:
    template <class F>
    static Work* New(F&& f) {
      using D = std::decay_t<F>;
      void* p = ::operator new(AllocSize<F>());
      assert(reinterpret_cast<uintptr_t>(p) % alignof(Work) == 0);
      Work* w = new (p) Work;
      w->invoke_ = &Work::Invoke<D>;
      new (w->Trailer<D>()) D(std::forward<F>(f));
      return w;
    }

    // Called exactly once.
    void Destroy() {
      assert(next_.load(std::memory_order_relaxed) == Sealed());
      size_t size = *Trailer<size_t>();
      this->~Work();
      ::operator delete(this, size);
    }

    // Called exactly once for every instance of Work except the very last one.
    void ContinueWith(Work* next) {
      // Invariant: _next is either null or sealed.
      if (Work* w = next_.exchange(next, std::memory_order_acq_rel)) {
        static_cast<void>(w);
        assert(w == Sealed());
        Destroy();
        while ((next = next->Run())) {}
      }
    }

    // Called exactly once.
    Work* Run() {
      invoke_(this);
      Work* next = next_.exchange(Sealed(), std::memory_order_acq_rel);
      if (next) {
        assert(next != Sealed());
        Destroy();
      }
      return next;
    }

   private:
    Work() {}

    static Work* Sealed() { return reinterpret_cast<Work*>(alignof(Work)); }

    template <class T>
    T* Trailer() {
      uintptr_t n = reinterpret_cast<uintptr_t>(this) + sizeof(Work);
      return reinterpret_cast<T*>(n + (-n & (alignof(T) - 1)));
    }

    template <class F>
    static constexpr size_t AllocSize() {
      constexpr size_t S = std::max(sizeof(F), sizeof(size_t));
      constexpr size_t A = std::max(alignof(F), alignof(size_t));
      constexpr size_t P = A > alignof(Work) ? A - alignof(Work) : 0;
      return sizeof(Work) + P + S;
    }

    // Called exactly once.
    template <class F>
    static void Invoke(Work* w) {
      F* f = w->Trailer<F>();
      std::move(*f)();
      f->~F();
      *w->Trailer<size_t>() = AllocSize<F>();
    }

    std::atomic<Work*> next_{nullptr};
    void (*invoke_)(Work*);
  };

  std::atomic<Work*> tail_{Work::New([] {})};
};

}  // namespace romkatv

#endif  // ROMKATV_ACTION_CHAIN_ACTION_CHAIN_H
