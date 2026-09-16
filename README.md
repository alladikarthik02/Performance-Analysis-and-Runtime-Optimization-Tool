# hotpath

A CPU sampling profiler for Linux/AArch64, built from scratch — no `perf`, no
`libunwind`, no external dependencies. A `SIGPROF` timer interrupts the target
~100×/sec; a hand-written, async-signal-safe unwinder walks the AArch64
frame-pointer chain to capture the call stack; samples land in a lock-free ring
buffer and are drained afterwards for analysis.

```
  setitimer(ITIMER_PROF, 10ms)          CPU-time clock, not wall-clock
            │  every 10 ms of CPU
            ▼
  kernel freezes the process, saves all registers
            │  SA_SIGINFO hands the handler a ucontext
            ▼
  signal handler                         async-signal-safe: no malloc, no locks
    ├── pc = uc.pc                       which function is running
    ├── fp = uc.regs[29]                 head of the frame-record chain
    ├── unwind_fp(pc, fp, bounds)        walk x29 → [saved_x29, saved_x30]
    └── ring.try_push(frames)            lock-free; drop-and-count on overflow
            │
            ▼
  kernel restores registers, the program resumes mid-instruction
            ⋮
  stop(): disarm timer → clear global → restore handler → drain
```

The split is deliberate: the in-process half does the bare minimum legal inside
a signal handler, and every expensive step is deferred to normal context.

## Status

Built and tested:

| Component | What it does |
|---|---|
| `src/sampler.cpp`, `include/hotpath/sampler.h` | `SIGPROF`/`ITIMER_PROF` timer, signal handler, start/stop lifecycle |
| `include/hotpath/unwind.h` | async-signal-safe AArch64 frame-pointer unwinder |
| `include/hotpath/ring_buffer.h` | lock-free single-producer/single-consumer ring buffer |

Not yet built: **symbolization** (turning captured addresses into function
names) and the **optimization advisor**. The tool currently captures raw
addresses and stack depth. See *Known limitations* for why `dladdr` was ruled
out and what replaces it.

## Build and test

Everything builds and runs inside a container (Ubuntu 24.04, aarch64) — the
profiler depends on Linux signal delivery, the AArch64 ABI, and ELF, none of
which exist on macOS.

```sh
./scripts/dev.sh bash -c "cmake -B build -S . -G Ninja \
    && cmake --build build \
    && ctest --test-dir build --output-on-failure"
```

`scripts/dev.sh` builds the image on first use and bind-mounts the repo, so
edits happen on the host and compilation happens on Linux.

Requires C++17 and CMake ≥ 3.16. Targets that will be profiled must be compiled
with `-fno-omit-frame-pointer` (see *Known limitations*).

## Design decisions

One constraint governs the whole design: **the capture path runs inside a signal
handler, which does not run on a separate thread — it hijacks the thread it
interrupts.** That forbids allocation, locks, and most of libc, because the
handler can interrupt code mid-operation and then deadlock against it.

Almost every decision below is derivable from that one fact.

### A hand-written frame-pointer unwinder, not `backtrace()`

`backtrace()` works — a spike confirmed it — but it is **not async-signal-safe**.
It routes through `_Unwind_Backtrace`, which may allocate and takes a lock while
parsing `.eh_frame`. A profiling signal can land *inside* `malloc` while it holds
its arena lock; if the handler then wants that same lock, it blocks forever on a
lock only the code it just froze can release. That is the classic profiler hang,
and it is nondeterministic.

Walking the AAPCS64 frame record is pure memory loads. Each function's prologue
stores a pair — `[x29+0]` = caller's frame pointer, `[x29+8]` = return address —
so the call stack is a linked list, and traversing it needs no locks, no
allocation, and no libc.

### A lock-free ring buffer, not a mutex

A mutex here is not slow, it is a **guaranteed self-deadlock**: if the signal
lands while the drain loop holds the lock, the handler blocks on a lock only the
thread it just interrupted can release. Single-producer/single-consumer is the
weakest structure that solves the problem, so the producer owns the write index,
the consumer owns the read index, and a release/acquire pair on those indices is
the entire synchronization.

Overflow policy is **drop and count** — never block (deadlock) and never grow
(`malloc`). The drop count is reported, because a profiler that silently
discards samples misrepresents its own coverage.

### `ITIMER_PROF`, not `ITIMER_REAL`

Wall-clock ticks keep accruing while a process is blocked or asleep, and would
blame that time on whatever happened to be on the stack when it went to sleep.
`ITIMER_PROF` counts user + system CPU time, so a sleeping process accrues zero
samples — the correct denominator for a tool that advises on CPU optimizations.

### Four unwinder safety gates

A corrupt frame pointer must never fault the profiled process, so every pointer
is validated before it is dereferenced:

1. non-null,
2. 16-byte aligned (required by AAPCS64 — cheap, high-yield garbage detection),
3. at or above the stack's low bound,
4. `fp + 16 <= high`, so **both** words fit.

The fourth is a real off-by-one trap. The natural `fp < high` passes when
`fp == high - 8`, and reading the second word then touches memory past the end of
the stack. Requiring room for both words is what prevents an out-of-bounds read.

Separately, each step requires the next frame pointer to be **strictly greater**
than the current one. The stack grows downward, so callers sit at higher
addresses — which makes this both an ABI-correctness check and a **termination
proof**: a strictly increasing sequence inside a bounded region is finite, so no
corrupt chain or cycle can loop forever.

## Measured results

Measured in-container on aarch64; reproduce with the commands above.

**Test suite — 18 tests across 3 binaries, all passing:**

| Suite | Tests | What it covers |
|---|---:|---|
| `ring_buffer` | 7 | round trip, FIFO order, overflow accounting, index wraparound, frame clamping, live two-thread concurrency |
| `unwind` | 10 | fault injection: misaligned fp, null fp, self-cycle, two-frame cycle, decreasing fp, boundary over-read, depth cap |
| `sampler` | 1 | end-to-end against a known call chain |

**Sampler, end-to-end.** Profiling a known chain `main → hp_top → hp_mid →
hp_leaf` at a 2 ms period over ~1 s of CPU:

```
collected 499 samples, 0 dropped, 499 total ticks
self-time leader: hp_leaf (496)   multiframe samples: 499
hp_leaf: self=496 incl=496 | hp_top incl=496 | full-chain=496
```

499 ticks against an expected 500 (1000 ms ÷ 2 ms), zero drops, and the busy
leaf correctly identified as the hot spot in 496 of 499 samples — while still
unwinding up the chain to `hp_top` in the same 496.

**Sanitizers.** Correctness claims are verified by tooling, not by a lucky run:

- **ThreadSanitizer** on the ring buffer: 7/7 pass with **zero data races** over
  200,000 cross-thread operations. This is what validates the release/acquire
  memory-ordering contract — lock-free bugs are timing-dependent and can hide
  for millions of runs, so a passing run alone proves nothing.
- **AddressSanitizer + UBSan** on the unwinder: 10/10 pass. Because the
  fault-injection tests build their fake stacks in heap arrays, ASan knows the
  exact bounds — so a wrong safety gate would surface as a heap-buffer-overflow.
  This is what upgrades "the tests pass" to "the bounds checks are proven."
- **AddressSanitizer** on the live signal-handler path: clean.

## Known limitations

Stated plainly, because a profiler that knows what it cannot see is worth more
than one that quietly misreports.

**A frameless leaf makes its own caller invisible.** A leaf function that calls
nothing gets no frame record even under `-fno-omit-frame-pointer`, so `x29` still
points at its caller's frame and the leaf→caller return link lives only in the
`x30` register. The innermost frame is still correct (it comes from the program
counter), but the immediate caller is skipped. Using `x30` unconditionally would
be worse — it would invent a duplicate frame whenever the innermost function
*did* build a record. A correct fix needs CFI-based unwinding.

This is visible in the run above: `hp_mid` is absent. It is a property of how the
*target* was compiled, not of the profiler — at `-O2` the leaf is frameless and
the caller is skipped; at `-O1` or under ASan the leaf builds a frame and the
caller reappears.

**Requires frame pointers.** Frame-pointer unwinding cannot walk through code
compiled without them, most notably optimized system libraries. This trades
coverage of foreign code for safety and simplicity in ours — the right side of
that trade for a tool advising on your own hot functions.

**Symbolization is not implemented.** `dladdr` was disqualified by spike: it
resolves only against `.dynsym`, which by definition contains no `static`
(internal-linkage) functions — often exactly the hot code you most need named.
The replacement parses the ELF `.symtab` directly, keeping each function as an
address range so that an address in a gap resolves to nothing rather than being
misattributed to the previous function.

**Inlining distorts attribution.** You must profile optimized code, but
optimization erases the structure you want to attribute time to — an inlined
function has no frame and can vanish from the stack entirely, folded into its
caller. Recovering the logical inline stack requires reading
`DW_TAG_inlined_subroutine` from the debug info.

**Multi-threaded targets warn rather than report.** Process-wide `SIGPROF` is
delivered to an arbitrary thread, so the interrupted thread is frequently not the
one burning CPU. The tool detects this and says so instead of emitting
plausible-looking numbers.

**Sampling is statistical.** It cannot report exact call counts, and work shorter
than the sampling period can be missed entirely. It finds what is *consistently*
hot, not rare short spikes.

## Layout

```
include/hotpath/   ring_buffer.h, unwind.h, sampler.h
src/               sampler.cpp
tests/             test.h (dependency-free harness) + 3 suites
docs/              SPEC.md, CHALLENGES.md, CHALLENGE_LOG.md
scratch/           spike1.c — the de-risking spike cited by the docs
scripts/dev.sh     run any command inside the container
Dockerfile         Ubuntu 24.04 + toolchain, gdb, valgrind, binutils
```

## Documentation

- **`docs/SPEC.md`** — full architecture, the spike results that drove it, and a
  closing section that attacks its own assumptions.
- **`docs/CHALLENGES.md`** — engineering challenges with the reasoning behind
  each, written to be defended out loud.
- **`docs/CHALLENGE_LOG.md`** — every bug hit during the build in plain English:
  symptom, hypotheses (including the wrong ones), root cause, fix, and what it
  generalizes to.
