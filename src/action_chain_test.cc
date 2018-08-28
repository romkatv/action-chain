#include "action_chain.h"

#include <stddef.h>
#include <stdint.h>

#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

int main() {
  static constexpr size_t kActions = 128 << 20;
  static constexpr size_t kThreads = 128;
  static constexpr size_t kActionsPerThread = kActions / kThreads;
  static_assert(kActionsPerThread * kThreads == kActions);

  uint64_t counter = 0;
  auto start = std::chrono::high_resolution_clock::now();
  {
    std::mutex m;
    romkatv::ActionChain action_chain;
    std::vector<std::thread> threads;
    for (size_t i = 0; i != kThreads; ++i) {
      threads.emplace_back([&] {
        for (size_t i = 0; i != kActionsPerThread; ++i) {
          auto f = [&] { ++counter; };
          action_chain.Add(f);
        }
      });
    }
    for (std::thread& t : threads) t.join();
  }
  auto end = std::chrono::high_resolution_clock::now();

  if (counter != kActions) {
    std::cerr << "TEST FAILURE" << std::endl;
    return 1;
  }

  std::chrono::duration<double> d = end - start;
  std::cerr << "Actions: " << kActions << std::endl;
  std::cerr << "Threads: " << kThreads << std::endl;
  std::cerr << "Actions per thread: " << kActionsPerThread << std::endl;
  std::cerr << "Total wall time (s): " << d.count() << std::endl;
  std::cerr << "Actions per second: " << kActions / d.count() << std::endl;
  std::cerr << "Time per action (ns): " << 1e9 * d.count() / kActions << std::endl;
}
