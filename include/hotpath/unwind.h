// hotpath — async-signal-safe AArch64 frame-pointer unwinder.
//
// WHY WE HAND-ROLL THIS INSTEAD OF CALLING backtrace()
// ----------------------------------------------------
// backtrace() works (spike proved it) but is NOT async-signal-safe: it routes
// through _Unwind_Backtrace, which can dlopen/malloc on first use and takes a
// lock while parsing .eh_frame. A SIGPROF tick lands at an ARBITRARY
// instruction -- including inside malloc holding its arena lock. If the handler
// then wants that same lock: self-deadlock, nondeterministically. That is the
// classic profiler hang and the worst kind of bug to ship.
//
// Walking the AArch64 frame record is instead PURE LOADS: no locks, no
// allocation, no libc. Trivially signal-safe.
//
// THE AAPCS64 FRAME RECORD
// ------------------------
// A prologue does `stp x29, x30, [sp, #-N]!; mov x29, sp`, so the frame pointer
// x29 points at a pair of 64-bit words:
//     [x29 + 0] = caller's saved frame pointer (x29)
//     [x29 + 8] = caller's saved link register  (x30 = return address)
// Walking the stack is chasing that linked list until a sanity gate stops us.
//
// COST, STATED PLAINLY (SPEC 3.2 / 8.2): requires -fno-omit-frame-pointer on the
// target and cannot unwind through frame-pointer-less code (e.g. optimized
// system libs). We trade coverage-of-foreign-code for safety-in-our-code. For a
// tool advising on YOUR hot functions, which you compile, that is the right side.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>  // __builtin_memcpy path / std::memcpy

namespace hotpath {

// Half-open address range [low, high) of the target thread's stack. Every memory
// read the unwinder performs is gated against this, so a corrupt frame pointer
// can never make us dereference unmapped memory and fault the target. In
// production these bounds come from the thread's stack mapping (Task 4); in
// tests they bound a hand-built fake stack.
struct StackBounds {
  std::uintptr_t low;
  std::uintptr_t high;
};

// Read one 64-bit word from an address already validated to be in-bounds.
// memcpy avoids a strict-aliasing violation and lowers to a single load; it is a
// compiler builtin here, so it is async-signal-safe.
inline std::uintptr_t load_word(std::uintptr_t addr) noexcept {
  std::uintptr_t v;
  std::memcpy(&v, reinterpret_cast<const void*>(addr), sizeof(v));
  return v;
}

// A frame pointer is walkable iff it is non-null, 16-byte aligned (AAPCS64
// stack alignment), and leaves room to read BOTH words [fp, fp+16) inside the
// stack. The `fp + 16 <= high` form (not `fp < high`) is deliberate: we read a
// word at fp AND at fp+8, so accepting fp in (high-16, high) would over-read the
// second word past the stack and segfault. That is a real off-by-one this guard
// exists to prevent.
inline bool walkable_fp(std::uintptr_t fp, StackBounds b) noexcept {
  return fp != 0 && (fp & 0xf) == 0 && fp >= b.low && fp + 16 <= b.high;
}

// The pure stack walk. Records the interrupted PC as frame 0, then follows the
// frame-record chain recording each caller's return address. Writes up to
// `max_frames` PCs into `out` and returns the count.
//
// GUARANTEES (all verified by fault-injection tests, no signals needed):
//   * never allocates, never locks, never faults on a corrupt chain;
//   * always terminates -- each step requires a strictly-greater fp within a
//     bounded region, so the sequence is finite;
//   * degrades to {pc} when fp is unwalkable (leaf/prologue/corruption), rather
//     than guessing.
//
// NOTE on frame PCs >= 1: these are RETURN addresses (the instruction after the
// call). For function-level attribution that is correct; a symbolizer wanting
// precise line info would use pc-1 for caller frames (SPEC 8.5 -- we do not
// claim line-level attribution, so we leave the raw return address here).
//
// NOTE on the leaf case (SPEC 8.2): we seed frame 0 from `pc` directly, which is
// always correct for the innermost frame. We do NOT use the register LR to
// refine the immediate-caller-during-prologue case, because doing so naively
// double-counts the caller in the common (frame-already-set-up) case. That is a
// documented v1 bias, not an oversight.
inline std::size_t unwind_fp(std::uintptr_t pc, std::uintptr_t fp,
                             StackBounds bounds, std::uint64_t* out,
                             std::size_t max_frames) noexcept {
  if (max_frames == 0) return 0;

  std::size_t n = 0;
  out[n++] = static_cast<std::uint64_t>(pc);  // frame 0 = interrupted PC, always valid

  std::uintptr_t cur = fp;
  while (n < max_frames && walkable_fp(cur, bounds)) {
    const std::uintptr_t next = load_word(cur);      // caller's saved fp
    const std::uintptr_t ret = load_word(cur + 8);   // caller's return address
    out[n++] = static_cast<std::uint64_t>(ret);

    // Progress + sanity: the next frame must be strictly higher on the stack.
    // Equal-or-lower means corruption or a cycle; stop rather than loop or trust
    // it. This single check is what makes termination provable.
    if (next <= cur) break;
    cur = next;
  }
  return n;
}

}  // namespace hotpath
