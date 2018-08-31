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
    Mem(Mem&& other) : p_(std::exchange(other.p_, nullptr)) {}
    ~Mem() { ::operator delete(p_, kAllocSize); }
    Mem& operator=(Mem&& other) {
      p_ = std::exchange(other.p_, nullptr);
      return *this;
    }

   private:
    friend class ActionChain;
    explicit Mem(void* p) : p_(p) {}

    void* p_;
  };

  ActionChain() { Work::RunAll(tail_.load(std::memory_order_relaxed)); }
  ActionChain(ActionChain&&) = delete;
  ~ActionChain() {
    Work* p = tail_.load(std::memory_order_acquire);
    p->Destroy();
    ::operator delete(p, kAllocSize);
  }

  // Either executes `action` synchronously (in which case some other actions added
  // concurrently by other threads may also run synchronously after `f` returns)
  // or schedules it for execution after all previously scheduled actions have
  // completed.
  //
  // Actions are guaranteed to run in the same order they were added.
  //
  // By passing `mem` you allow Run() to reuse heap-allocated memory. It may speed
  // things up. Note that Mem is not thread safe. You must not pass the same instance
  // of Mem concurrently to multiple Run() calls.
  //
  // Example:
  //
  //   // These counters are updated atomically after every request.
  //   uint64_t total_requests = 0;
  //   uint64_t failed_requests = 0;
  //   ActionChain mutex;
  //
  //   // Thread-safe.
  //   void SendRequests(uint64_t n) {
  //     ActionChain::Mem mem;
  //     while (n--) {
  //       bool ok = SendRequest();
  //       // Note that the counters may be updated after SendRequest() returns.
  //       mutex.Run(&mem, [=] {
  //         ++total_requests;
  //         if (!ok) ++failed_requests;
  //       });
  //     }
  //   }
  template <class F>
  void Run(Mem* mem, F&& action) {
    assert(mem);
    if (!mem->p_) mem->p_ = ::operator new(kAllocSize);
    Work* work = Work::New(mem->p_, std::forward<F>(action));
    mem->p_ = tail_.exchange(work, std::memory_order_acq_rel)->ContinueWith(work);
  }

  template <class F>
  void Run(F&& action) {
    Run(&mem_, std::forward<F>(action));
  }

 private:
  static constexpr std::size_t kAllocSize = 32;

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
      assert(next_.load(std::memory_order_relaxed) != nullptr);
      this->~Work();
    }

    // Called exactly once for every instance of Work except the very last one.
    // Returns null or raw memory of kAllocSize bytes.
    void* ContinueWith(Work* next) {
      assert(next != nullptr && next != Sealed());
      if (Work* w = next_.load(std::memory_order_acquire)
                        ?: next_.exchange(next, std::memory_order_acq_rel)) {
        static_cast<void>(w);
        assert(w == Sealed());
        Destroy();
        RunAll(next);
        return this;
      }
      return nullptr;
    }

    static void RunAll(Work* w) {
      assert(w != nullptr && w != Sealed());
      w->invoke_(w);
      if (Work* next = w->next_.exchange(Sealed(), std::memory_order_acq_rel)) {
        assert(next != Sealed());
        RunAllSlow(w, next);
      }
    }

   private:
    Work() {}

    static Work* Sealed() { return reinterpret_cast<Work*>(alignof(Work)); }

    static void RunAllSlow(Work* w, Work* next);

    // Called exactly once.
    template <class F>
    static void Invoke(Work* w) {
      assert(w != nullptr && w != Sealed());
      F& f = *reinterpret_cast<F*>(w + 1);
      std::move(f)();
      f.~F();
    }

    std::atomic<Work*> next_{nullptr};
    void (*invoke_)(Work*);
  };

  static thread_local Mem mem_;

  std::atomic<Work*> tail_{Work::New(::operator new(kAllocSize), [] {})};
};

}  // namespace romkatv

#endif  // ROMKATV_ACTION_CHAIN_ACTION_CHAIN_H
