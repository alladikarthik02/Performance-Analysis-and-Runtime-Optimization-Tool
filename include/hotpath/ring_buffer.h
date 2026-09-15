// hotpath — lock-free SPSC ring buffer for profile samples.
//
// WHY LOCK-FREE IS NOT A STYLE CHOICE HERE
// ----------------------------------------
// The producer is a SIGPROF handler. A signal handler does not run on "another
// thread" -- it *hijacks the very thread* that would be holding a lock. So:
//
//     main thread: lock(m)  ... <SIGPROF arrives> ...
//     handler:     lock(m)  -> blocks forever, waiting on a lock that only the
//                              thread it just interrupted can release.
//
// That is a hard self-deadlock, and it is nondeterministic -- it only happens
// when the timer lands inside the critical section. A mutex here is not a slow
// design, it is a broken one. Same argument rules out malloc (its arena lock),
// printf (stdio lock), and anything else that can block.
//
// So the whole buffer is: preallocated at init, fixed capacity, plain atomics.
//
// MEMORY ORDERING CONTRACT
// ------------------------
// Classic SPSC. The producer publishes with a release store to write_idx_; the
// consumer reads it with acquire. That pairing is what guarantees the consumer
// cannot observe an incremented index before the slot bytes it refers to. The
// indices are free-running u64 counters (never wrapped); only the *slot lookup*
// masks. At 100 samples/sec a u64 counter overflows in ~5.8 billion years, so
// wraparound is not a case we handle.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>  // std::abort in the power-of-two guard
#include <memory>

namespace hotpath {

// Depth cap for a single captured stack. 64 is deep enough for real call chains
// while keeping a Sample at 520 bytes, so the default buffer stays ~2 MB.
inline constexpr std::size_t kMaxFrames = 64;

struct Sample {
  std::uint16_t n_frames;
  std::uint64_t pc[kMaxFrames];
};

// Cache-line padding to keep the producer's write_idx_ and the consumer's
// read_idx_ off the same line. Apple silicon uses 128-byte lines; x86 and most
// AArch64 use 64. 128 is the safe over-estimate -- padding too much costs bytes,
// padding too little costs false sharing on every single sample.
inline constexpr std::size_t kCacheLine = 128;

class RingBuffer {
 public:
  // capacity MUST be a power of two: the slot index is computed with a mask
  // (idx & (capacity-1)) rather than a modulo, because integer division in a
  // signal handler is pure waste on the hot path.
  explicit RingBuffer(std::size_t capacity)
      : capacity_(capacity),
        mask_(capacity - 1),
        slots_(new Sample[capacity]()) {
    // Not an assert: a non-power-of-two mask silently corrupts every index, and
    // this is cheap and runs once at init.
    if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
      std::abort();
    }
  }

  // --- producer side: ONLY ever called from the signal handler -------------
  //
  // async-signal-safe: no allocation, no locks, no libc. Just loads, stores,
  // and a bounded memcpy into a preallocated slot.
  //
  // Returns false when the buffer is full. Policy is DROP AND COUNT, never
  // block and never grow: blocking would deadlock (see header comment) and
  // growing would malloc. A profiler that silently discards samples is lying
  // about its own coverage; one that reports "I dropped 3.2%" is an instrument.
  bool try_push(const std::uint64_t* pcs, std::uint16_t n) noexcept {
    if (n > kMaxFrames) n = kMaxFrames;

    // relaxed is correct for our own index: this is the only producer, so no
    // other writer can race us on it.
    const std::uint64_t w = write_idx_.load(std::memory_order_relaxed);
    // acquire pairs with the consumer's release store in try_pop, so a slot the
    // consumer has finished with is genuinely free before we reuse it.
    const std::uint64_t r = read_idx_.load(std::memory_order_acquire);

    if (w - r >= capacity_) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    Sample& s = slots_[w & mask_];
    s.n_frames = n;
    for (std::uint16_t i = 0; i < n; ++i) s.pc[i] = pcs[i];

    // release: everything written to the slot above must be visible to any
    // consumer that observes this new index. This store is the publish point.
    write_idx_.store(w + 1, std::memory_order_release);
    return true;
  }

  // --- consumer side: called from normal code, timer stopped --------------
  bool try_pop(Sample& out) noexcept {
    const std::uint64_t r = read_idx_.load(std::memory_order_relaxed);
    const std::uint64_t w = write_idx_.load(std::memory_order_acquire);
    if (r == w) return false;  // empty

    const Sample& s = slots_[r & mask_];
    out.n_frames = s.n_frames;
    for (std::uint16_t i = 0; i < s.n_frames; ++i) out.pc[i] = s.pc[i];

    // release: signals to the producer that this slot is reusable.
    read_idx_.store(r + 1, std::memory_order_release);
    return true;
  }

  std::uint64_t dropped() const noexcept {
    return dropped_.load(std::memory_order_relaxed);
  }
  std::uint64_t pushed() const noexcept {
    return write_idx_.load(std::memory_order_relaxed);
  }
  std::size_t size() const noexcept {
    return static_cast<std::size_t>(write_idx_.load(std::memory_order_acquire) -
                                    read_idx_.load(std::memory_order_acquire));
  }
  std::size_t capacity() const noexcept { return capacity_; }

 private:
  const std::size_t capacity_;
  const std::size_t mask_;
  std::unique_ptr<Sample[]> slots_;

  alignas(kCacheLine) std::atomic<std::uint64_t> write_idx_{0};
  alignas(kCacheLine) std::atomic<std::uint64_t> read_idx_{0};
  alignas(kCacheLine) std::atomic<std::uint64_t> dropped_{0};
};

}  // namespace hotpath
