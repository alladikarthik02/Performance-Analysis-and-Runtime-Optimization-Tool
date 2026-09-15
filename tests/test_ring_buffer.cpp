// Tests for the lock-free SPSC ring buffer.
//
// The buffer's job is to never lie: every sample that reports "pushed" must come
// back out intact and in order, and every sample that could not fit must be
// counted as dropped. These tests attack exactly those two promises, plus the
// two mechanical places an SPSC ring breaks: index wraparound and a real
// concurrent producer/consumer.
#include "hotpath/ring_buffer.h"

#include <thread>
#include <vector>

#include "test.h"

using hotpath::RingBuffer;
using hotpath::Sample;

// Build a stack whose PCs are a known function of a seed, so the consumer can
// verify byte-for-byte that it got the exact sample the producer wrote -- a
// torn or mis-slotted sample fails this immediately.
static void fill(std::uint64_t* pcs, std::uint16_t n, std::uint64_t seed) {
  for (std::uint16_t i = 0; i < n; ++i) pcs[i] = (seed << 8) | i;
}
static bool matches(const Sample& s, std::uint16_t n, std::uint64_t seed) {
  if (s.n_frames != n) return false;
  for (std::uint16_t i = 0; i < n; ++i)
    if (s.pc[i] != ((seed << 8) | i)) return false;
  return true;
}

TEST(push_then_pop_roundtrip) {
  RingBuffer rb(8);
  std::uint64_t pcs[4];
  fill(pcs, 4, 0xAB);
  CHECK(rb.try_push(pcs, 4));
  Sample out{};
  CHECK(rb.try_pop(out));
  CHECK(matches(out, 4, 0xAB));
  CHECK_EQ(rb.dropped(), 0u);
}

TEST(empty_pop_returns_false) {
  RingBuffer rb(8);
  Sample out{};
  CHECK(!rb.try_pop(out));
}

TEST(fifo_order_preserved) {
  RingBuffer rb(8);
  std::uint64_t pcs[2];
  for (std::uint64_t k = 0; k < 5; ++k) {
    fill(pcs, 2, k);
    CHECK(rb.try_push(pcs, 2));
  }
  for (std::uint64_t k = 0; k < 5; ++k) {
    Sample out{};
    CHECK(rb.try_pop(out));
    CHECK(matches(out, 2, k));  // came back in the order pushed
  }
}

// The core overflow promise: a capacity-N buffer holds exactly N in-flight
// samples; the (N+1)th push must fail and increment the drop counter. Getting
// the boundary wrong (holding N-1, or "succeeding" then corrupting) is the
// classic ring-buffer bug.
TEST(overflow_drops_and_counts) {
  RingBuffer rb(4);
  std::uint64_t pcs[1];
  for (std::uint64_t k = 0; k < 4; ++k) {
    fill(pcs, 1, k);
    CHECK(rb.try_push(pcs, 1));  // 4 fit
  }
  fill(pcs, 1, 99);
  CHECK(!rb.try_push(pcs, 1));  // 5th is refused
  CHECK(!rb.try_push(pcs, 1));  // and again
  CHECK_EQ(rb.dropped(), 2u);   // both counted, not silently lost

  // The 4 that fit are still intact and in order -- overflow must not corrupt
  // the samples already in the buffer.
  for (std::uint64_t k = 0; k < 4; ++k) {
    Sample out{};
    CHECK(rb.try_pop(out));
    CHECK(matches(out, 1, k));
  }
}

// Free-running u64 indices are masked to slots. If we push/pop far more than
// capacity, the indices sweep past `capacity` many times -- this proves the
// mask math stays correct across wraparound of the slot mapping.
TEST(index_wraparound_stays_correct) {
  RingBuffer rb(4);
  std::uint64_t pcs[3];
  for (std::uint64_t k = 0; k < 1000; ++k) {
    fill(pcs, 3, k);
    CHECK(rb.try_push(pcs, 3));  // push one
    Sample out{};
    CHECK(rb.try_pop(out));      // pop one -> never full, never empty
    CHECK(matches(out, 3, k));   // exact sample, 1000 times across the wrap
  }
  CHECK_EQ(rb.dropped(), 0u);
}

TEST(clamps_overlong_frame_count) {
  RingBuffer rb(4);
  std::vector<std::uint64_t> big(hotpath::kMaxFrames + 10, 0x1234);
  // Asking to push more than kMaxFrames must clamp, not overrun the slot.
  CHECK(rb.try_push(big.data(), static_cast<std::uint16_t>(big.size())));
  Sample out{};
  CHECK(rb.try_pop(out));
  CHECK_EQ(out.n_frames, static_cast<std::uint16_t>(hotpath::kMaxFrames));
}

// The real thing: one producer thread, one consumer thread, no locks. This is
// what the signal handler + drain loop do, minus the signal. We assert the
// strong property -- every sample consumed is a valid, untorn sample, and the
// accounting (consumed + still-queued + dropped) conserves every attempt.
TEST(concurrent_spsc_no_torn_samples) {
  RingBuffer rb(1024);
  constexpr std::uint64_t kAttempts = 200000;

  std::uint64_t produced = 0, dropped = 0;
  std::thread producer([&] {
    std::uint64_t pcs[8];
    for (std::uint64_t k = 0; k < kAttempts; ++k) {
      fill(pcs, 8, k);
      if (rb.try_push(pcs, 8))
        ++produced;
      else
        ++dropped;
    }
  });

  std::uint64_t consumed = 0;
  bool torn = false;
  std::thread consumer([&] {
    Sample out{};
    // Keep draining until the producer is done AND the buffer is empty.
    while (consumed + rb.dropped() < kAttempts || rb.size() > 0) {
      if (rb.try_pop(out)) {
        // Every sample must be well-formed: 8 frames, self-consistent PCs.
        // We can't check the seed order (drops create gaps) but a torn read
        // would corrupt the internal (pc[i] == (seed<<8)|i) relationship.
        std::uint64_t seed = out.pc[0] >> 8;
        if (!matches(out, 8, seed)) torn = true;
        ++consumed;
      }
    }
  });

  producer.join();
  consumer.join();

  CHECK(!torn);                              // no sample was ever torn
  CHECK_EQ(produced, consumed);              // everything pushed was popped
  CHECK_EQ(produced + dropped, kAttempts);   // nothing vanished
  CHECK_EQ(rb.dropped(), dropped);           // counter agrees with the producer
}

RUN_ALL()
