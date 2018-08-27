// Copyright (c) 2018 Roman Perepelitsa.
//
// TODO: Figure out whether memory order constraints can be relaxed.

#ifndef ROMKATV_ACTION_CHAIN_ACTION_CHAIN_H_
#define ROMKATV_ACTION_CHAIN_ACTION_CHAIN_H

#include <stddef.h>

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

namespace romkatv {

class ActionChain {
 public:
  ActionChain() { tail_.load()->Run(); }

  ActionChain(ActionChain&&) = delete;

  ~ActionChain() {
    return;
    std::mutex m;
    std::condition_variable c;
    bool done = false;
    Add([&] {
      std::lock_guard lock(m);
      done = true;
      c.notify_one();
    });
    {
      std::unique_lock lock(m);
      c.wait(lock, [&] { return done; });
    }
    Work* w = tail_.load();
    w->Wait();
    w->Destroy();
  }

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
      size_t pad = alignof(D) > alignof(Work) ? alignof(D) - alignof(Work) : 0;
      char* p = new char[sizeof(Work) + pad + sizeof(D)];
      assert(reinterpret_cast<uintptr_t>(p) % alignof(Work) == 0);
      Work* w = new (p) Work;
      w->invoke_ = &Work::Invoke<D>;
      new (Align(p + sizeof(Work), alignof(D))) D(std::forward<F>(f));
      return w;
    }

    void Wait() {
      while (!next_.load(std::memory_order_acquire)) std::this_thread::yield();
    }

    void Destroy() {
      char* p = reinterpret_cast<char*>(this);
      this->~Work();
      delete[] p;
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

    static void* Align(void* p, size_t alignment) {
      uintptr_t n = reinterpret_cast<uintptr_t>(p);
      return reinterpret_cast<void*>(n + (-n & (alignment - 1)));
    }

    template <class F>
    void Invoke() {
      void* p = Align(reinterpret_cast<char*>(this) + sizeof(Work), alignof(F));
      auto* f = static_cast<F*>(p);
      std::move (*f)();
      f->~F();
    }

    std::atomic<Work*> next_{nullptr};
    void (Work::*invoke_)();
  };

  std::atomic<Work*> tail_{Work::New([] {})};
};

}  // namespace romkatv

#endif  // ROMKATV_ACTION_CHAIN_ACTION_CHAIN_H
