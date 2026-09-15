// hotpath — the sampler: SIGPROF timer -> signal handler -> unwinder -> ring.
//
// This is where the three pieces compose. The design is dominated by one fact:
// a signal handler receives only (sig, siginfo, ucontext) -- it cannot be handed
// a `this`. So the active sampler is found through a single atomic global, and
// everything the handler touches must be async-signal-safe:
//   * reading ucontext fields         -- plain loads
//   * unwind_fp()                      -- pure loads (see unwind.h)
//   * ring.try_push()                  -- atomics, preallocated (see ring_buffer.h)
// No malloc, no locks, no libc-with-locks (no printf) on that path. All the
// expensive setup (stack-bounds discovery, thread counting) happens in start(),
// in normal context, and is cached.
#pragma once

#include <csignal>
#include <cstdint>
#include <string>
#include <vector>

#include "hotpath/ring_buffer.h"
#include "hotpath/unwind.h"

namespace hotpath {

class Sampler {
 public:
  struct Config {
    unsigned period_us = 10000;         // 10 ms; see SPEC 3.1 / 5 for the rationale
    std::size_t buffer_capacity = 1u << 15;  // 32768 samples (power of two, ~17 MB)
  };

  // Two constructors rather than one with a `Config cfg = Config()` default
  // argument. You cannot use a *nested* class's default member initializers in a
  // default argument of the enclosing class's member function -- at that point
  // Sampler is still incomplete, so Config's in-class initializers aren't
  // available yet (g++ 13: "default member initializer ... required before the
  // end of its enclosing class"). The delegating default ctor is defined in the
  // .cpp, where both classes are complete. Full write-up: docs/CHALLENGES.md B7.
  Sampler();                     // uses default Config
  explicit Sampler(Config cfg);
  ~Sampler();

  Sampler(const Sampler&) = delete;
  Sampler& operator=(const Sampler&) = delete;

  // Arm ITIMER_PROF and install the SIGPROF handler. Caches stack bounds and
  // warns (once) if the process is multi-threaded, per SPEC 8.3 -- process-wide
  // SIGPROF is delivered to an arbitrary thread, so multi-threaded attribution
  // would be a plausible lie. v1 warns rather than fabricates.
  void start();

  // Disarm the timer, restore the previous handler, and drain the ring buffer
  // into samples(). Safe to call once after start().
  void stop();

  // --- results, valid after stop() ---
  const std::vector<Sample>& samples() const { return samples_; }
  std::uint64_t dropped() const { return ring_.dropped(); }
  std::uint64_t total_ticks() const { return ring_.pushed() + ring_.dropped(); }
  unsigned period_us() const { return cfg_.period_us; }
  const std::string& warnings() const { return warnings_; }

  // Exposed for the file writer (Task 5).
  StackBounds stack_bounds() const { return bounds_; }

 private:
  // The SIGPROF entry point. Static because sigaction needs a plain function
  // pointer; it recovers state from the atomic global.
  static void handler(int sig, siginfo_t* si, void* ucontext);

  Config cfg_;
  RingBuffer ring_;
  StackBounds bounds_{0, 0};
  struct sigaction old_sa_{};
  bool running_ = false;
  std::vector<Sample> samples_;
  std::string warnings_;
};

}  // namespace hotpath
