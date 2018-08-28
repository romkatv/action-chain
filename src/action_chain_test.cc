// Usage: action_chain_test [OPTION]...
//
// Example: action_chain_test --sync=ActionChain --threads=8 --ops-per-action=128
//
// Options:
//
//   --sync=SYNC           synchronization primitive
//   --threads=NUM         number of threads running synchronized actions
//   --ops-per-action=NUM  number of primitive operations per action
//   --actions=NUM         total number of actions for all threads; zero value means
//                         default, which depends on other flags
//
// Synchronization primitives:
//
//   ActionChain           ActionChain class from this library with explicit Mem
//                         passing by the caller
//   ActionChainNoPooling  ActionChain class from this library without explicit
//                         Mem passing by the caller
//   CriticalSection       regular mutex
//   Unsynchronized        no synchronization; set --threads=1 when you use this
//
// All numbers must be integers with an optional prefix:
//
//   K  multiply by 2^10
//   M  multiply by 2^20
//   M  multiply by 2^30

#include "action_chain.h"

#include <sys/resource.h>
#include <sys/time.h>

#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#define CHECK(cond...)                                                      \
  if (!(cond)) {                                                            \
    ::std::fprintf(stderr, "FATAL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ::std::abort();                                                         \
  } else                                                                    \
    (void)0

namespace romkatv {
namespace {

struct Flags {
  std::string sync = "ActionChain";
  std::uint64_t threads = 1;
  std::uint64_t ops_per_action = 1;
  // The default value is set in ParseFlags as it depends on other flags.
  std::uint64_t actions = 0;
};

void ParseFlag(std::uint64_t* flag, std::string_view s) {
  CHECK(!s.empty());
  int shift = 0;
  switch (std::tolower(s.back())) {
    case 'k':
      shift = 10;
      break;
    case 'm':
      shift = 20;
      break;
    case 'g':
      shift = 30;
      break;
  }
  if (shift) s.remove_suffix(1);
  CHECK(std::from_chars(s.data(), s.data() + s.size(), *flag).ec == std::errc());
  CHECK(((*flag) << shift >> shift) == *flag);
  *flag <<= shift;
}

void ParseFlag(std::string* flag, std::string_view s) { *flag = std::string(s); }

Flags ParseFlags(const char* const* begin, const char* const* end) {
  auto Match = [&](const char* name, auto* flag) -> bool {
    if (std::strncmp(*begin + 2, name, std::strlen(name))) return false;
    const char* val = *begin + 2 + std::strlen(name);
    if (*val == '\0') {
      ++begin;
      CHECK(begin != end);
      val = *begin;
    } else if (*val == '=') {
      ++val;
    } else {
      return false;
    }
    ParseFlag(flag, val);
    return true;
  };
  Flags res;
  for (; begin != end; ++begin) {
    CHECK(!std::strncmp(*begin, "--", 2));
    CHECK(Match("sync", &res.sync) || Match("actions", &res.actions) ||
          Match("threads", &res.threads) || Match("ops-per-action", &res.ops_per_action));
  }
  if (!res.actions) {
    res.actions = (128 / (res.ops_per_action / 32 + 1)) << 20;
  }

  return res;
}

class ActionChainNoPooling {
 public:
  using Mem = int;

  template <class F>
  void Run(Mem*, F&& f) {
    action_chain_.Run(std::move(f));
  }

 private:
  ActionChain action_chain_;
};

class CriticalSection {
 public:
  using Mem = int;

  template <class F>
  void Run(Mem*, F&& f) {
    std::lock_guard lock(mutex_);
    std::move(f)();
  }

 private:
  std::mutex mutex_;
};

class Unsynchronized {
 public:
  using Mem = int;

  template <class F>
  void Run(Mem*, F&& f) {
    std::move(f)();
  }
};

double CpuTimeSec() {
  auto ToSec = [](const timeval& tv) { return tv.tv_sec + 1e-6 * tv.tv_usec; };
  rusage usage = {};
  CHECK(getrusage(RUSAGE_SELF, &usage) == 0);
  return ToSec(usage.ru_utime) + ToSec(usage.ru_stime);
}

template <class Sync>
int Benchmark(const Flags& flags) {
  const std::uint64_t actions_per_thread = flags.actions / flags.threads;
  CHECK(actions_per_thread * flags.threads == flags.actions);

  auto Out = []() -> std::ostream& { return std::cout << std::setw(28) << std::left; };

  auto PrintNum = [&](const char* name, std::uint64_t val) {
    auto Try = [&](int shift, const char* suffix) {
      if ((val >> shift << shift) != val) return false;
      std::cout << (val >> shift) << suffix << std::endl;
      return true;
    };
    Out() << name;
    if (!Try(30, "G") && !Try(20, "M") && !Try(10, "K")) {
      std::cout << val << std::endl;
    }
  };

  Out() << "Synchronization primitive" << flags.sync << std::endl;
  PrintNum("Threads", flags.threads);
  PrintNum("Ops per action", flags.ops_per_action);
  PrintNum("Actions", flags.actions);
  PrintNum("Actions per thread", actions_per_thread);

  volatile std::uint64_t counter = 0;
  auto wall_time_start = std::chrono::high_resolution_clock::now();
  double cpu_time_start = CpuTimeSec();
  {
    std::mutex m;
    Sync sync;
    std::vector<std::thread> threads;
    for (std::uint64_t i = 0; i != flags.threads; ++i) {
      threads.emplace_back([&] {
        typename Sync::Mem mem;
        for (std::uint64_t i = 0; i != actions_per_thread; ++i) {
          sync.Run(&mem, [&] {
            for (std::uint64_t j = 0; j != flags.ops_per_action; ++j) ++counter;
          });
        }
      });
    }
    for (std::thread& t : threads) t.join();
  }
  double cpu_time_end = CpuTimeSec();
  auto wall_time_end = std::chrono::high_resolution_clock::now();

  if (counter != flags.ops_per_action * flags.actions) {
    std::cerr << "TEST FAILURE" << std::endl;
    return 1;
  }

  double wall = std::chrono::duration<double>(wall_time_end - wall_time_start).count();
  double cpu = cpu_time_end - cpu_time_start;
  Out() << "Total wall time (s)" << wall << std::endl;
  Out() << "Actions per wall second" << flags.actions / wall << std::endl;
  Out() << "Wall time per action (ns)" << 1e9 * wall / flags.actions << std::endl;
  Out() << "Total CPU time (s)" << cpu << std::endl;
  Out() << "Actions per CPU second" << flags.actions / cpu << std::endl;
  Out() << "CPU time per action (ns)" << 1e9 * cpu / flags.actions << std::endl;

  return 0;
}

int BenchmarkMain(int argc, char* argv[]) {
  Flags flags = ParseFlags(argv + 1, argv + argc);
  std::unordered_map<std::string, int (*)(const Flags&)> bm = {
      {"ActionChain", Benchmark<ActionChain>},
      {"ActionChainNoPooling", Benchmark<ActionChainNoPooling>},
      {"CriticalSection", Benchmark<CriticalSection>},
      {"Unsynchronized", Benchmark<Unsynchronized>},
  };
  CHECK(bm[flags.sync]);
  return bm[flags.sync](flags);
}

}  // namespace
}  // namespace romkatv

int main(int argc, char* argv[]) { return romkatv::BenchmarkMain(argc, argv); }
