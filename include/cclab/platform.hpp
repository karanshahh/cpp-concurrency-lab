#pragma once
// platform.hpp - small portability helpers: cache-line size, clock, clock resolution,
// spin hint, thread pinning, and a "is this a valid benchmark build?" check.

#include <chrono>
#include <cstddef>
#include <cstdint>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define CCLAB_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define CCLAB_SANITIZED 1
#endif
#endif

namespace cclab {

// Distance that avoids false sharing. Apple Silicon has 128-byte cache lines; most x86 parts have 64.
// We use 128 on aarch64 so padding is right on the Mac and in the aarch64 VM. We do not use
// std::hardware_destructive_interference_size: its value is baked into the ABI and GCC warns about it.
#if defined(__aarch64__)
inline constexpr std::size_t kCacheLine = 128;
#else
inline constexpr std::size_t kCacheLine = 64;
#endif

// Monotonic nanosecond clock (steady_clock never goes backwards).
inline std::uint64_t now_ns() noexcept {
  using namespace std::chrono;
  return static_cast<std::uint64_t>(
      duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

// Smallest observed positive step of now_ns(): the clock's real resolution on this machine.
// Per-operation times close to this value cannot be trusted below it.
inline std::uint64_t clock_resolution_ns() noexcept {
  std::uint64_t best = ~std::uint64_t{0};
  for (int i = 0; i < 10000; ++i) {
    const std::uint64_t a = now_ns();
    std::uint64_t b = now_ns();
    while (b == a) b = now_ns();
    if (b - a < best) best = b - a;
  }
  return best;
}

// Hint to the CPU that we are in a spin-wait loop.
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#endif
}

// Pin the calling thread to one CPU. Linux only; false elsewhere or on failure.
inline bool pin_this_thread(unsigned cpu) noexcept {
#if defined(__linux__)
  if (cpu >= CPU_SETSIZE) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
  (void)cpu;
  return false;
#endif
}

// nullptr if this build is valid for benchmarking, otherwise a warning to print.
inline const char* build_warning() noexcept {
#if !defined(NDEBUG)
  return "NOT a release build (NDEBUG is not defined): numbers are not valid";
#elif defined(CCLAB_SANITIZED)
  return "built with a sanitizer: numbers are not valid";
#else
  return nullptr;
#endif
}

}  // namespace cclab
