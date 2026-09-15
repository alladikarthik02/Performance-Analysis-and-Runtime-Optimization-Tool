# `hotpath` — Tech Spec

A sampling profiler and optimization advisor for Linux/AArch64, built from scratch.
C++ core (sampling, unwinding, symbolization) + Python analysis layer (attribution,
advice, reports).

> Status: living document. §8 records holes found while poking at this spec.
> Every claim in §2 was verified by an executable spike before being written down.

---

## 1. What we are building, and what we are deliberately NOT building

**Goal.** Point the tool at a running program. Learn where its time goes — not just
"which function", but *which call path*. Then have it argue, from evidence, where an
optimization (inlining, caching) would pay, and prove the argument by applying it and
measuring.

**The one-line honesty test for this project:** a profiler that shells out to `perf`
is a *wrapper*. This tool must produce its own samples, walk its own stacks, and
resolve its own symbols. If `perf` vanished, `hotpath` still works.

**Non-goals** (each a real cost, named up front so they don't read as oversights):

| Not doing | Why |
|---|---|
| Kernel-mode / off-CPU profiling | Requires `perf_event_open` + `CAP_PERFMON`; unavailable under Docker Desktop's LinuxKit VM. We profile **on-CPU user time**, and say so. |
| Hardware counters (cache misses, IPC) | Same gate. A cache-miss-driven advisor would be better; it is honestly out of reach here. |
| DWARF-based unwinding (`.eh_frame` CFI) | We require frame pointers instead. See §3.2 — this is a real trade, not laziness. |
| Full DWARF line/inline resolution (v1) | `DW_TAG_inlined_subroutine` parsing is a project unto itself. v1 attributes to the *physical* function and reports inlining as a known distortion (§8.1). |
| Multi-threaded targets (v1) | `ITIMER_PROF` delivery to an arbitrary thread makes per-thread attribution wrong. Design accommodates it (§3.3); v1 ships single-threaded and *fails loudly* rather than lying. |

---

## 2. Verified ground truth (spike results, not assumptions)

Every one of these was executed in the container before the architecture was fixed.
The spike lives at `scratch/spike1.c`.

| # | Question | Answer | Consequence |
|---|---|---|---|
| Q1 | Does `setitimer(ITIMER_PROF)` + `SIGPROF` fire? | **Yes** (62 ticks / ~3s CPU) | Timer backend is viable. |
| Q2 | Does `backtrace()` return sane frames in a handler? | **Yes** (6 frames) | But see §3.2 — "works" ≠ "safe". |
| Q3 | Can `dladdr()` resolve a **static** function? | **NO** | **`dladdr` is disqualified.** Must parse ELF `.symtab`. |
| Q4 | Is the static fn in `.symtab`? | **It was inlined away entirely** | Inlining destroys attribution (§8.1). |
| Q5 | Is PIE load bias real and computable? | **Yes**, bias = `0xaaaae6770000` | Symbolizer must subtract bias. |
| Q6 | Does DWARF record inlining? | **Yes**, 11 × `DW_TAG_inlined_subroutine` | An escape hatch exists for v2. |

**Q3, in detail, because it is the load-bearing finding.**
The captured stack's innermost frame was `0xaaaae6770d84`, reported `<UNRESOLVED>`.
`readelf --syms` shows:

```
addr=0x0d58  size=64  bind=LOCAL  handler
```

`handler` spans `[0xd58, 0xd98)`; `0xd84` lies inside it. `dladdr` failed because
`handler` is `bind=LOCAL` — and **`dladdr` resolves only against `.dynsym`, which by
definition contains no local symbols.** Even compiling with `-rdynamic` (the most
favourable case) does not help: `-rdynamic` promotes *global* symbols into `.dynsym`,
never internal-linkage ones.

Since real hot code is routinely `static` (translation-unit-private helpers), a
`dladdr`-based symbolizer would print `<UNRESOLVED>` for exactly the functions the
user most needs named. **This is why §3.4 exists.**

**Q5, in detail.** The same run gives the bias for free:
`0xaaaae6770d84 (runtime) − 0xd84 (link-time) = 0xaaaae6770000`.
Cross-checked against `main`: runtime `0xaaaae6770eb0` − bias = `0xeb0`, which lies in
`main`'s `[0xdd0, 0x11a8)`. Two independent frames agree ⇒ formula validated.

---

## 3. Architecture

```
                     ┌──────────── target process ────────────┐
   SIGPROF (10ms) ──►│ signal handler                         │
                     │   └─ FP unwinder (async-signal-safe)   │
                     │        └─ writes N PCs ──► ring buffer │  (lock-free, preallocated)
                     └────────────────────┬───────────────────┘
                                          │ drained at exit (or when full)
                                          ▼
                                    .hpprof file        (raw PCs + load bias + metadata)
                                          │
                     ┌────────────────────▼───────────────────┐
                     │ symbolizer: ELF .symtab + load bias    │  C++
                     └────────────────────┬───────────────────┘
                                          ▼
                     ┌────────────────────────────────────────┐
                     │ Python: attribution → advisor → report │
                     └────────────────────────────────────────┘
```

**Why the split at the `.hpprof` file?** The in-process half must be tiny, allocation-free,
and signal-safe. Analysis wants hash maps, sorting, and statistics — everything forbidden
in a signal handler. Serializing raw PCs and doing all thinking offline is what makes the
hot path defensible. It is also why the analysis half is Python: it is not on the hot path.

### 3.1 Sampling clock — why `ITIMER_PROF`

| Option | Measures | Verdict |
|---|---|---|
| `ITIMER_REAL` | wall time | Catches I/O blocking, but attributes sleep to whoever is on the stack. Wrong for a CPU optimizer. |
| **`ITIMER_PROF`** | **user+sys CPU** | **Chosen.** We advise on CPU optimizations, so we must sample CPU time. A sleeping process must not accrue samples. |
| `timer_create` + `CLOCK_THREAD_CPUTIME_ID` | per-thread CPU | Strictly better for multi-threaded. v2 (§8.3). |

**Period: 10 ms default.** Rationale in §5 (statistics) — shorter periods buy resolution
at the cost of overhead and skid; 10 ms with a run of ≥1s gives ≥100 samples, enough for
the confidence intervals we intend to print.

### 3.2 Unwinding — why a hand-written frame-pointer walker, not `backtrace()`

The spike proves `backtrace()` *works*. We are not going to use it, and the reason is
the most important safety argument in this codebase.

**`backtrace()` is not async-signal-safe.** It routes through `_Unwind_Backtrace`
(libgcc), which on first call may `dlopen`/`malloc` and which takes a lock while
parsing `.eh_frame`. A profiling signal arrives at an *arbitrary* instruction — including
while the program is inside `malloc` holding its arena lock. If the handler then calls a
function that wants that same lock: **self-deadlock**. This is not theoretical; it is the
classic profiler hang, and it is nondeterministic, which makes it the worst possible bug
to ship.

**The alternative: walk the AArch64 frame record ourselves.**
AAPCS64 defines a frame record: `x29` (FP) points to a pair `[saved_x29, saved_x30]`,
i.e. caller's FP at `[x29]` and return address (LR) at `[x29+8]`. Walking that chain is
**pure loads** — no locks, no allocation, no libc. Trivially async-signal-safe.

Starting state comes from the third `sigaction` argument, `ucontext_t`:
- `uc->uc_mcontext.pc` → the interrupted PC (innermost frame)
- `uc->uc_mcontext.regs[29]` → FP
- `uc->uc_mcontext.regs[30]` → LR (needed for leaf functions, §8.2)

**The cost, stated plainly:** this requires `-fno-omit-frame-pointer` on the target and
cannot unwind through frame-pointer-less code (most notably optimized system libraries).
`backtrace()`/DWARF would handle those. We trade *coverage of foreign code* for
*safety and simplicity in our code*. For a tool advising on **your** hot functions —
which you compile — that is the correct side of the trade.

**Sanity requirements while walking** (a corrupt FP must never fault the target):
1. FP must be 16-byte aligned (AAPCS64 stack alignment).
2. FP must strictly increase (stacks grow down ⇒ caller frames are at higher addresses).
   This also guarantees **termination** — no cycle can loop forever.
3. FP must lie inside the thread's stack bounds.
4. Depth capped at `kMaxFrames` (64).

### 3.3 The ring buffer — lock-free by necessity, not fashion

The handler is a **producer that interrupts its own consumer's thread**. A mutex is
therefore not merely slow, it is a **guaranteed deadlock**: if the signal lands while the
main thread holds the lock, the handler blocks forever on a lock only that thread can
release.

Design: single-producer / single-consumer, **preallocated at init**, fixed capacity.
- Producer (handler): claim a slot via `fetch_add` on a write index, fill it, publish
  with a `release` store to a per-slot sequence number.
- Consumer (drain at exit): `acquire`-load the sequence to see a fully-written slot.
- **Overflow policy: drop and count.** Never block, never grow (no `malloc` in a handler).
  The drop count is reported. A profiler that silently discards data is lying; one that
  says "I dropped 3.2% of samples" is an instrument.

### 3.4 Symbolizer — ELF `.symtab`, forced by Q3

Per §2/Q3, `dladdr` is out. We parse the ELF ourselves:

1. `dl_iterate_phdr` → each loaded object's name + `dlpi_addr` (**the load bias**).
2. `mmap` the ELF file; read `Elf64_Ehdr` → section headers → find `SHT_SYMTAB`
   (falling back to `SHT_DYNSYM` for stripped objects, with reduced fidelity).
3. Keep every `STT_FUNC` symbol with `st_size > 0` as `[st_value, st_value + st_size)`.
4. Resolve: `link_addr = runtime_pc − bias`; binary-search the sorted range table.

Using `st_size` (rather than nearest-preceding-symbol, as `dladdr` does) means an address
in a gap between functions resolves to **nothing** instead of being misattributed to the
previous function. Correct-and-unknown beats confidently-wrong.

### 3.5 Analysis & advisor (Python)

Attribution from the sampled stacks:
- **self time** = samples where the function is the *innermost* frame.
- **inclusive time** = samples where it appears *anywhere* on the stack (counted once
  per sample — recursion must not double-count; dedupe frames per sample).
- **hot path** = the highest-weight root→leaf chain in the merged call tree.

Advisor rules (v1), each gated on statistical significance (§5):
- **Inline candidate**: high call count × small body (`st_size` from ELF) × meaningful
  inclusive time. Prologue/epilogue overhead dominates a small callee.
- **Cache/memoize candidate**: high call count + high self time + repeated arguments.
  **Honest caveat:** sampling cannot give call counts or argument values at all. This
  requires the instrumentation backend (§8.4) and, for arguments, opt-in annotation.
  v1 will not fabricate this; it reports what it can prove.

---

## 4. File format (`.hpprof`)

Binary, little-endian, versioned. Written once at drain; never parsed in-process.

```
header:  magic "HPRF" | u32 version | u32 period_us | u64 n_samples | u64 n_dropped
objects: u32 count, then per object: u64 bias, u16 path_len, path bytes
samples: per sample: u16 n_frames, then n_frames × u64 pc
```

Raw PCs, not names: symbolization is a *pure function* of (PC, bias, ELF), so doing it
offline keeps the handler trivial and makes profiles re-symbolizable.

---

## 5. Statistics — why this profiler prints error bars

A sampling profiler is an estimator, and pretending otherwise is how people "optimize"
noise. Samples in a function are Binomial(N, p); for a function with `k` of `N` samples,
`p̂ = k/N` and the standard error is `sqrt(p̂(1−p̂)/N)`. With `k` small the relative error
is roughly `1/sqrt(k)` — **9 samples means ±33%.**

Consequences we will enforce:
- Report a 95% CI (Wilson interval — it does not misbehave near p≈0, unlike normal
  approximation) alongside every percentage.
- **The advisor must refuse to advise** when a candidate's CI overlaps the noise floor.
  "This function is 4% ± 5%" is not an optimization target.

---

## 6. Testing strategy

The hard part: how do you test a profiler, whose output is *nondeterministic by design*?

1. **Unit-test the pure pieces deterministically.** Ring buffer (SPSC ordering, overflow
   accounting), symbolizer (known ELF → known ranges), attribution math (synthetic stacks
   → exact self/inclusive), Wilson CI (known values).
2. **Fault-injection for the unwinder.** Hand-built fake stacks: misaligned FP, cyclic FP,
   FP outside stack bounds, null FP. Assert we stop cleanly and never fault.
3. **Ground-truth targets for the sampler.** A program with a *known* time split (e.g. a
   calibrated 80/20 between two `noinline` functions). Assert the profile recovers 80/20
   **within the CI** — a statistical assertion, not an exact one. This is the only honest
   way to test a sampler.
4. **Differential check against ground truth** for the symbolizer: our resolution of every
   symbol vs. `readelf` output for the same binary. `readelf` is the oracle.

---

## 7. Task breakdown (build order)

Ordered so each task is independently testable, and the riskiest thing comes first.

| # | Task | Why here |
|---|---|---|
| 0 | Docker env + toolchain pin | **done** — everything else is unreproducible without it |
| 1 | Spikes: SIGPROF / backtrace / dladdr / ELF | **done** — killed the `dladdr` design before it was written |
| 2 | Ring buffer (SPSC, lock-free) + unit tests | Pure data structure; testable with zero signals. |
| 3 | FP unwinder + fault-injection tests | Riskiest code in the project. Test with fake stacks, no timer. |
| 4 | Timer/sampler wiring (`SIGPROF` → unwind → ring) | Composes 2+3; first end-to-end samples. |
| 5 | `.hpprof` writer + reader | Freezes the contract between C++ and Python. |
| 6 | ELF symbolizer + differential test vs `readelf` | Turns PCs into names. |
| 7 | Python attribution (self/inclusive/hot path) + CI math | |
| 8 | Advisor + report | |
| 9 | Validation harness (apply suggestion, measure speedup) | The résumé's second bullet. |

**Today's scope: tasks 0–4** (+ CHALLENGES log). 5–9 are tomorrow.

---

## 8. Holes in this spec (found by attacking it)

Written *after* drafting §3, by trying to break it.

### 8.1 Inlining destroys attribution — the deepest problem here
Q4 proved it: at `-O1`, GCC inlined `hot_static_function` **and** `hot_extern_function`
into `main`. The stack contained neither. A profiler reporting "100% of time in `main`"
is technically true and completely useless.

This is the fundamental tension: **you must profile optimized code** (profiling `-O0` tells
you about a program you will never ship), **but optimization erases the structure you want
to attribute to.**

Options: (a) parse `DW_TAG_inlined_subroutine` and synthesize virtual inline frames — what
`perf` and VTune do, correct, expensive; (b) require `-fno-inline` on targets — destroys
the thing being measured; (c) **v1: attribute to the physical function, detect the
distortion, and warn.** We can *detect* it: if a function's `st_size` is large while its
DWARF says it inlined others, attribution is suspect. Shipping (c) with an explicit
warning, and (a) named as v2. **A profiler that knows what it cannot see is worth more
than one that quietly misattributes.**

### 8.2 Frameless leaves — the leaf's *caller* is the frame that's lost
**Confirmed empirically in Task 4 (see CHALLENGES B8), and the precise statement
is sharper than the usual hand-wave.** A frame record is written by the prologue;
a leaf function that calls nothing gets **no frame record at all**, even under
`-fno-omit-frame-pointer` (GCC emits `sub sp,…` for locals but no `stp x29,x30`).

So when SIGPROF lands in a frameless leaf:
- frame 0 is still correct — we seed it from `uc_mcontext.pc`, so the **leaf
  itself is captured**;
- but `x29` was never updated by the leaf, so it points at the **caller's** frame,
  and the leaf→caller return link is sitting only in the live `x30` (LR) register;
- walking from `x29` therefore reads the caller's *own* saved-LR, i.e. the return
  into the **caller's caller** — so the immediate caller is **skipped**.

Observed: chain `hp_top → hp_mid → hp_leaf`, sampled in the frameless leaf
`hp_leaf`, produced stacks `[hp_leaf, hp_top, …]` — `hp_mid` (the leaf's caller)
missing in 100% of samples.

**Why v1 does not "fix" it with the LR.** Unconditionally using `regs[30]` as
frame 1 would double-count the caller whenever the innermost function *did* set up
a frame (the common case), inventing a frame that isn't real. A wrong frame is
worse than a missing one (§8.1's principle). Correctly gating on "is pc in a
frameless leaf / mid-prologue?" needs instruction inspection or CFI — the v2
unwinder. **v1 ships the characterized bias, pinned by a test assertion, not a
guess.**

### 8.3 `ITIMER_PROF` + threads = wrong answers
POSIX delivers the process-wide `SIGPROF` to an *arbitrary* thread. With N threads, the
handler frequently unwinds a thread that was **not** burning the CPU that triggered the
tick. v1 must **detect** multi-threading and refuse/warn rather than emit a plausible lie.
v2: `timer_create(CLOCK_THREAD_CPUTIME_ID, SIGEV_THREAD_ID)` per thread.

### 8.4 The advisor's "caching" rule has no data source
§3.5 asks for call counts and argument values. **Sampling provides neither.** This hole is
in the résumé bullet itself, so it must be answered honestly: a second backend
(`-finstrument-functions`, giving exact enter/exit counts) is required for inline advice,
and argument-value profiling needs opt-in annotation. v1 ships inline advice gated on the
instrumentation backend; memoization advice is v2. **The alternative — guessing — is worse
than not shipping it.**

### 8.5 Sampling skid
The PC at signal delivery is not exactly the PC that consumed the time (interrupt latency,
OOO execution). Skid is ~µs-scale; our period is 10 ms, so per-function attribution is
fine, but **per-line attribution would be a lie.** We therefore do not offer line-level
attribution.

### 8.6 We are measuring under a VM
Docker Desktop runs a LinuxKit VM under macOS's Virtualization.framework. Absolute
timings are noisier than bare metal, and the host scheduler can steal time. Mitigation:
report *relative* attribution (%), require repeated runs for the speedup claims in task 9,
and never present absolute ns as if it were bare-metal truth.
