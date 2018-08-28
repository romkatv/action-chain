#ifndef ROMKATV_ACTION_CHAIN_ACTION_CHAIN_H_
#define ROMKATV_ACTION_CHAIN_ACTION_CHAIN_H

#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
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
  class Mem {
   public:
    Mem() : p_(nullptr) {}
    Mem(Mem&& other) : p_(other.p_) { other.p_ = nullptr; }
    ~Mem() {
      // This `if` has no effect on the semantics of the program but it does
      // make it faster.
      if (p_) ::operator delete(p_, kAllocSize);
    }
    Mem& operator=(Mem&& other) {
      if (this != &other) {
        p_ = other.p_;
        other.p_ = nullptr;
      }
      return *this;
    }

   private:
    friend class ActionChain;

    explicit Mem(void* p) : p_(p) {}

    void* Release() {
      void* res = p_;
      p_ = nullptr;
      return res;
    }

    void* p_;
  };

  ActionChain() { Work::RunAll(tail_.load()); }
  ActionChain(ActionChain&&) = delete;
  ~ActionChain() {
    Work* p = tail_.load();
    p->Destroy();
    ::operator delete(p, kAllocSize);
  }

  // Either executes `action` synchronously (in which case some other actions added
  // concurrently by other threads may also run synchronously after `f` returns)
  // or schedules it for execution after all previously scheduled actions have
  // completed.
  //
  // Actions are guaranteed to run in the same order they were added.
  template <class F>
  Mem Run(Mem&& mem, F&& action) {
    void* p = mem.p_ ? mem.Release() : ::operator new(kAllocSize);
    Work* work = Work::New(p, std::forward<F>(action));
    return Mem(tail_.exchange(work, std::memory_order_acq_rel)->ContinueWith(work));
  }

  template <class F>
  Mem Run(F&& action) {
    return Run(Mem(), std::move(action));
  }

 private:
  static constexpr std::size_t kAllocSize = 64;

  class Work {
   public:
    template <class F>
    static Work* New(void* p, F&& f) {
      // This is easy to fix with no adverse effects for the code that currently
      // compiles.
      static_assert(alignof(F) <= alignof(Work), "Sorry, not implemented");
      // This might be a bit trickier to fix without slowing down existing code.
      static_assert(sizeof(Work) + sizeof(F) <= kAllocSize);
      Work* w = new (p) Work;
      w->invoke_ = &Work::Invoke<std::decay_t<F>>;
      new (w + 1) std::decay_t<F>(std::forward<F>(f));
      return w;
    }

    // Called exactly once.
    void Destroy() {
      assert(next_.load(std::memory_order_relaxed) == Sealed());
      this->~Work();
    }

    // Called exactly once for every instance of Work except the very last one.
    void* ContinueWith(Work* next) {
      if (Work* w = next_.exchange(next, std::memory_order_acq_rel)) {
        static_cast<void>(w);
        assert(w == Sealed());
        Destroy();
        RunAll(next);
        return this;
      }
      return nullptr;
    }

    // TODO: Move the loop into an out-of-line function and leave only the hot path
    // in here.
    static void RunAll(Work* w) {
      assert(w != nullptr && w != Sealed());
      while (true) {
        w->invoke_(w);
        if (Work* next = w->next_.exchange(Sealed(), std::memory_order_acq_rel)) {
          assert(next != Sealed());
          w->Destroy();
          w = next;
        } else {
          break;
        }
      }
    }

   private:
    Work() {}

    static Work* Sealed() { return reinterpret_cast<Work*>(alignof(Work)); }

    // Called exactly once.
    template <class F>
    static void Invoke(Work* w) {
      F& f = *reinterpret_cast<F*>(w + 1);
      std::move(f)();
      f.~F();
    }

    std::atomic<Work*> next_{nullptr};
    void (*invoke_)(Work*);
  };

  std::atomic<Work*> tail_{Work::New(::operator new(kAllocSize), [] {})};
};

}  // namespace romkatv

#endif  // ROMKATV_ACTION_CHAIN_ACTION_CHAIN_H
