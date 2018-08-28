#ifndef ROMKATV_ACTION_CHAIN_ACTION_CHAIN_H_
#define ROMKATV_ACTION_CHAIN_ACTION_CHAIN_H

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
  ActionChain() { Work::RunAll(tail_.load()); }
  ~ActionChain() { tail_.load()->Destroy(); }
  ActionChain(ActionChain&&) = delete;

  // Either executes `action` synchronously (in which case some other actions added
  // concurrently by other threads may also run synchronously after `f` returns)
  // or schedules it for execution after all previously scheduled actions have
  // completed.
  //
  // Actions are guaranteed to run in the same order they were added.
  template <class F>
  void Run(F&& action) {
    Work* work = Work::New(std::forward<F>(action));
    tail_.exchange(work, std::memory_order_acq_rel)->ContinueWith(work);
  }

 private:
  class Work {
   public:
    template <class F>
    static Work* New(F&& f) {
      static_assert(alignof(F) <= alignof(std::max_align_t));
      using D = std::decay_t<F>;
      void* p = ::operator new(AllocSize<D>());
      Work* w = new (p) Work;
      w->invoke_ = &Work::Invoke<D>;
      new (w->Trailer<D>()) D(std::forward<F>(f));
      return w;
    }

    // Called exactly once.
    void Destroy() {
      assert(next_.load(std::memory_order_relaxed) == Sealed());
      std::size_t size = *Trailer<std::size_t>();
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
        RunAll(next);
      }
    }

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

    static Work* Sealed() { return reinterpret_cast<Work*>(alignof(std::max_align_t)); }

    static constexpr std::size_t Align(std::size_t n) {
      return n + (-n & (alignof(std::max_align_t) - 1));
    }

    template <class T>
    T* Trailer() {
      return reinterpret_cast<T*>(reinterpret_cast<char*>(this) + Align(sizeof(Work)));
    }

    template <class F>
    static constexpr std::size_t AllocSize() {
      return Align(sizeof(Work)) + Align(sizeof(F));
    }

    // Called exactly once.
    template <class F>
    static void Invoke(Work* w) {
      F& f = *w->Trailer<F>();
      std::move(f)();
      f.~F();
      *w->Trailer<std::size_t>() = AllocSize<F>();
    }

    std::atomic<Work*> next_{nullptr};
    void (*invoke_)(Work*);
  };

  std::atomic<Work*> tail_{Work::New([] {})};
};

}  // namespace romkatv

#endif  // ROMKATV_ACTION_CHAIN_ACTION_CHAIN_H
