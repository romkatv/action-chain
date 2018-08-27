// TODO: Figure out whether memory order constraints can be relaxed.

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

    void Destroy() {
      size_t size = *Trailer<size_t>();
      this->~Work();
      ::operator delete(this, size);
    }

    // Called at most once.
    void ContinueWith(Work* next) {
      // Invariant: _next is either null or sealed.
      if (Work* w = next_.exchange(next, std::memory_order_acq_rel)) {
        static_cast<void>(w);
        assert(w == Sealed());
        Destroy();
        next->Run();
      }
    }

    // Called at most once.
    void Run() {
      Work* w = this;
      while (true) {
        (w->*w->invoke_)();
        // Invariant: w->_next is not sealed.
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

    template <class T>
    T* Trailer() {
      uintptr_t n = reinterpret_cast<uintptr_t>(this) + sizeof(Work);
      return reinterpret_cast<T*>(n + (-n & (alignof(T) - 1)));
    }

    template <class F>
    static size_t AllocSize() {
      constexpr size_t S = std::max(sizeof(F), sizeof(size_t));
      constexpr size_t A = std::max(alignof(F), alignof(size_t));
      size_t pad = A > alignof(Work) ? A - alignof(Work) : 0;
      return sizeof(Work) + pad + S;
    }

    template <class F>
    void Invoke() {
      F* f = Trailer<F>();
      std::move (*f)();
      f->~F();
      *Trailer<size_t>() = AllocSize<F>();
    }

    std::atomic<Work*> next_{nullptr};
    void (Work::*invoke_)();
  };

  std::atomic<Work*> tail_{Work::New([] {})};
};

}  // namespace romkatv

#endif  // ROMKATV_ACTION_CHAIN_ACTION_CHAIN_H
