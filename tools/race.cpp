// race.cpp - the classic shared-counter data race and its two fixes.
//
//   ./race race     2 threads, plain long, no lock   -> DATA RACE (undefined behaviour)
//   ./race mutex    same, protected by std::mutex
//   ./race atomic   same, std::atomic<long>
//
// Build it under ThreadSanitizer (./scripts/build.sh tsan) and run ./build-tsan/race race:
// TSan prints "WARNING: ThreadSanitizer: data race". With the mutex and atomic modes it is silent.
//
// NOTE: in an optimised build without sanitizers the compiler may fold the loop into one add and the
// race can become invisible. That is why "it printed the right number" proves nothing.

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace {

constexpr long kIters = 1'000'000;

long g_plain = 0;  // shared, unsynchronised
long g_locked = 0;
std::mutex g_mu;
std::atomic<long> g_atomic{0};

void work_race() {
  for (long i = 0; i < kIters; ++i) ++g_plain;  // read-modify-write on shared data, no sync: UB
}

void work_mutex() {
  for (long i = 0; i < kIters; ++i) {
    std::lock_guard<std::mutex> lk(g_mu);
    ++g_locked;
  }
}

void work_atomic() {
  // Relaxed is enough: we only need the increments to be atomic, not to order other memory.
  for (long i = 0; i < kIters; ++i) g_atomic.fetch_add(1, std::memory_order_relaxed);
}

template <class F>
void run_two(F f) {
  std::thread a(f);
  std::thread b(f);
  a.join();
  b.join();
}

}  // namespace

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "race";
  const long expected = 2 * kIters;
  long got = 0;

  if (std::strcmp(mode, "race") == 0) {
    run_two(work_race);
    got = g_plain;
  } else if (std::strcmp(mode, "mutex") == 0) {
    run_two(work_mutex);
    got = g_locked;
  } else if (std::strcmp(mode, "atomic") == 0) {
    run_two(work_atomic);
    got = g_atomic.load();
  } else {
    std::fprintf(stderr, "usage: race [race|mutex|atomic]\n");
    return 2;
  }

  const bool is_race = std::strcmp(mode, "race") == 0;
  const char* note = got != expected ? "(LOST UPDATES)"
                     : is_race       ? "(count matches, but this proves nothing: run it under TSan)"
                                     : "(correct)";
  std::printf("mode=%s expected=%ld got=%ld %s\n", mode, expected, got, note);
  return got == expected ? 0 : 1;
}
