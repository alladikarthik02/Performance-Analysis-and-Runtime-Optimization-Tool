// Fault-injection tests for the AArch64 frame-pointer unwinder.
//
// The unwinder's headline promise is "never faults on a corrupt chain, always
// terminates." You cannot prove that by profiling a healthy program -- you have
// to FEED it corruption on purpose. So each test builds a fake stack in a heap
// array and hands the unwinder a pointer into it, with bounds set to the array.
// Every nasty production scenario (misaligned fp, cycle, boundary over-read,
// null fp) becomes a deterministic, signal-free unit test.
#include "hotpath/unwind.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "test.h"

using hotpath::StackBounds;
using hotpath::unwind_fp;

// A 16-byte-aligned fake stack. Frame "slots" are 16 bytes (two 64-bit words):
// word 0 = saved caller fp, word 1 = saved return address. Slots are laid out at
// increasing addresses because the stack grows down -- caller frames live at
// HIGHER addresses, which is exactly what the unwinder's strictly-increasing
// gate expects.
struct FakeStack {
  std::vector<std::uint8_t> mem;
  std::uintptr_t base;  // 16-byte aligned
  std::uintptr_t end;

  explicit FakeStack(std::size_t slots) : mem((slots + 2) * 16, 0) {
    std::uintptr_t raw = reinterpret_cast<std::uintptr_t>(mem.data());
    base = (raw + 15) & ~std::uintptr_t(15);
    end = (raw + mem.size()) & ~std::uintptr_t(15);
  }
  StackBounds bounds() const { return {base, end}; }
  std::uintptr_t slot(std::size_t i) const { return base + i * 16; }

  // Write a frame record (saved_fp, saved_ret) at slot i.
  void put(std::size_t i, std::uintptr_t saved_fp, std::uintptr_t saved_ret) {
    std::uintptr_t a = slot(i);
    std::memcpy(reinterpret_cast<void*>(a), &saved_fp, 8);
    std::memcpy(reinterpret_cast<void*>(a + 8), &saved_ret, 8);
  }
};

// The happy path: a clean 3-deep chain must yield pc + 3 return addresses in
// order. This is also the oracle the corruption tests deviate from.
TEST(clean_chain_walks_fully) {
  FakeStack s(3);
  // bar -> foo -> main; signal hit in bar at pc 0xB00.
  s.put(0, s.slot(1), 0xB01);  // bar's record: caller fp = foo's slot, ret into foo
  s.put(1, s.slot(2), 0xB02);  // foo's record: caller fp = main's slot, ret into main
  s.put(2, 0, 0xB03);          // main's record: fp 0 terminates, ret into libc

  std::uint64_t out[16];
  std::size_t n = unwind_fp(0xB00, s.slot(0), s.bounds(), out, 16);

  CHECK_EQ(n, static_cast<std::size_t>(4));
  CHECK_EQ(out[0], static_cast<std::uint64_t>(0xB00));  // interrupted pc
  CHECK_EQ(out[1], static_cast<std::uint64_t>(0xB01));
  CHECK_EQ(out[2], static_cast<std::uint64_t>(0xB02));
  CHECK_EQ(out[3], static_cast<std::uint64_t>(0xB03));
}

// A null fp (leaf function that never set up a frame record, or the very top of
// the stack) must degrade to exactly the interrupted pc, not crash.
TEST(null_fp_yields_only_pc) {
  FakeStack s(1);
  std::uint64_t out[16];
  std::size_t n = unwind_fp(0xAA, 0, s.bounds(), out, 16);
  CHECK_EQ(n, static_cast<std::size_t>(1));
  CHECK_EQ(out[0], static_cast<std::uint64_t>(0xAA));
}

// A misaligned fp (not 16-byte aligned) is impossible under AAPCS64, so it means
// corruption. Reject it -> just the pc. (slot(0)+8 is 8-aligned, not 16.)
TEST(misaligned_fp_rejected) {
  FakeStack s(2);
  s.put(0, s.slot(1), 0xF1);
  std::uint64_t out[16];
  std::size_t n = unwind_fp(0xAA, s.slot(0) + 8, s.bounds(), out, 16);
  CHECK_EQ(n, static_cast<std::size_t>(1));  // walked nothing past pc
}

// fp below the stack region -> reject, no read.
TEST(fp_below_bounds_rejected) {
  FakeStack s(2);
  std::uint64_t out[16];
  std::size_t n = unwind_fp(0xAA, s.base - 16, s.bounds(), out, 16);
  CHECK_EQ(n, static_cast<std::size_t>(1));
}

// The boundary over-read case: fp so close to `high` that reading the SECOND
// word (fp+8) would touch [high, high+8). The `fp + 16 <= high` gate must reject
// this. If the gate were `fp < high`, this test would read past the array -- the
// exact segfault the guard exists to prevent.
TEST(fp_near_top_no_overread) {
  FakeStack s(2);
  std::uintptr_t fp = s.end - 8;  // room for one word, NOT two
  std::uint64_t out[16];
  std::size_t n = unwind_fp(0xAA, fp, s.bounds(), out, 16);
  CHECK_EQ(n, static_cast<std::size_t>(1));  // rejected, and crucially did not fault
}

// Self-cycle: a frame whose saved fp points at itself. Must record one return
// address then stop (next <= cur), never loop forever.
TEST(self_cycle_terminates) {
  FakeStack s(1);
  s.put(0, s.slot(0), 0xC1);  // fp -> itself
  std::uint64_t out[16];
  std::size_t n = unwind_fp(0xAA, s.slot(0), s.bounds(), out, 16);
  CHECK_EQ(n, static_cast<std::size_t>(2));  // pc + one ret, then the cycle gate fires
  CHECK_EQ(out[1], static_cast<std::uint64_t>(0xC1));
}

// Two-frame cycle A->B->A. The strictly-increasing gate breaks it: once we step
// "up" to B (higher addr) and B points back "down" to A, next <= cur stops us.
TEST(two_frame_cycle_terminates) {
  FakeStack s(2);
  s.put(0, s.slot(1), 0xD1);  // A -> B (up)
  s.put(1, s.slot(0), 0xD2);  // B -> A (down) : must terminate here
  std::uint64_t out[16];
  std::size_t n = unwind_fp(0xAA, s.slot(0), s.bounds(), out, 16);
  CHECK_EQ(n, static_cast<std::size_t>(3));  // pc, ret A, ret B, then stop
  CHECK_EQ(out[1], static_cast<std::uint64_t>(0xD1));
  CHECK_EQ(out[2], static_cast<std::uint64_t>(0xD2));
}

// A decreasing chain (next < cur) is corruption/non-progress; record the one
// frame we safely read, then stop.
TEST(decreasing_fp_stops) {
  FakeStack s(3);
  s.put(2, s.slot(1), 0xE2);  // start high, point lower -> must stop after one
  s.put(1, s.slot(0), 0xE1);
  std::uint64_t out[16];
  std::size_t n = unwind_fp(0xAA, s.slot(2), s.bounds(), out, 16);
  CHECK_EQ(n, static_cast<std::size_t>(2));
  CHECK_EQ(out[1], static_cast<std::uint64_t>(0xE2));
}

// Depth cap: a chain longer than max_frames must return exactly max_frames and
// not overrun the caller's buffer.
TEST(depth_cap_respected) {
  FakeStack s(10);
  for (std::size_t i = 0; i < 9; ++i)
    s.put(i, s.slot(i + 1), 0x100 + i);
  s.put(9, 0, 0x109);

  std::uint64_t out[4];
  std::size_t n = unwind_fp(0xAA, s.slot(0), s.bounds(), out, 4);
  CHECK_EQ(n, static_cast<std::size_t>(4));  // capped: pc + 3, not the full chain
}

// max_frames == 0 is a degenerate but legal request: write nothing.
TEST(zero_max_writes_nothing) {
  FakeStack s(1);
  std::uint64_t out[1] = {0xDEAD};
  std::size_t n = unwind_fp(0xAA, s.slot(0), s.bounds(), out, 0);
  CHECK_EQ(n, static_cast<std::size_t>(0));
  CHECK_EQ(out[0], static_cast<std::uint64_t>(0xDEAD));  // untouched
}

RUN_ALL()
