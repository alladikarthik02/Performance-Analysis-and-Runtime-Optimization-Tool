# hotpath — bug journal & engineering challenges

Every bug hit while building this, with the *reasoning*, not just the fix. Small
ones included on purpose (a missing `#include` is here next to a design-breaking
discovery) — the point is that reading this cold should make each failure legible,
and should be defensible out loud.

Format per entry: **Symptom → Hypotheses (including the wrong ones) → How I
isolated it → Root cause → Fix → Generalizes to.** Wrong hypotheses are kept
deliberately; disproving your own first guess is the story worth telling.

Legend: 🧱 environment/tooling · 🐛 code defect · 🔬 design-invalidating discovery

---

## B1 🧱 `docker build` fails: `docker-credential-desktop: executable file not found`

**Symptom.** First `docker build` died instantly:
```
error getting credentials - err: exec: "docker-credential-desktop": executable file not found in $PATH
```

**Hypotheses.**
- (H1) The Dockerfile is wrong. — *Rejected fast:* the build never reached line 1
  of the Dockerfile; it failed while resolving the base image's metadata, i.e.
  before any instruction ran. So the Dockerfile is not implicated.
- (H2) Docker isn't installed. — No; `docker --version` works. The *CLI* is fine.
- (H3) A credential **helper** the CLI wants to invoke isn't on PATH. — Most likely.

**How I isolated it.** Three checks, each confirming one link in the chain:
1. `cat ~/.docker/config.json` → `"credsStore": "desktop"`. So the CLI shells out
   to a helper binary named `docker-credential-desktop` for every registry op.
2. `which docker-credential-desktop` → not found. Confirms it's off PATH.
3. `ls /Applications/Docker.app/Contents/Resources/bin/` → the binary **is there**,
   just inside the app bundle, which a non-interactive shell's PATH doesn't include.

**Root cause.** Docker Desktop's GUI installer registers `/usr/local/bin/docker`
but keeps its credential helpers inside the `.app` bundle. `config.json` still
says `credsStore=desktop`, so the CLI tries to run a helper it can't find. It's a
PATH gap, not a Docker fault and not a config fault.

**Fix.** Prepend the bundle dir to PATH **for our commands only** — baked into
`scripts/dev.sh`. Deliberately did **not** edit `~/.docker/config.json`: that's the
user's global config, and deleting `credsStore` there would change auth behaviour
for every other project on the machine to work around a local PATH issue. We only
pull public images, so no credentials are actually needed anyway.

**Generalizes to.** "Executable not found" for a *helper* is almost never "install
the thing" — it's a PATH/plugin-discovery gap. And: prefer the narrowest fix that
touches only your own scope over mutating shared/global state.

---

## B2 🧱 macOS host: `fatal error: 'cstdio' file not found`

**Symptom.** The very first spike, compiled on the macOS host (before we moved to
Docker), couldn't find a *standard C++ header*, `<cstdio>`.

**Hypotheses.**
- (H1) Compiler broken / not installed. — No; `clang++ --version` is fine and C
  headers resolve. It's specifically the **C++** standard headers.
- (H2) The libc++ include path is wrong or pointing at a broken stub. — Likely,
  and this exactly matches a note already recorded in the `dslc` project's
  CMakeLists: some Command Line Tools installs ship a stub libc++ include dir while
  the real headers live under the SDK.

**How I isolated it.** `xcrun --show-sdk-path` → `/Library/.../MacOSX.sdk`, and
`ls "$SDK/usr/include/c++/v1/cstdio"` **exists**. So the headers are present; the
driver just isn't looking there by default.

**Root cause.** Partial CLT install: the driver's default libc++ include dir is a
broken stub; the SDK holds the real headers. Fixable with `-isysroot "$SDK"`.

**Fix — but really a pivot.** Rather than nurse `-isysroot` on the host, this bug
(plus no `perf`, SIP-blocked `dtrace`, and Mach-O instead of ELF) is *why the whole
project moved into Docker*. The host was fighting us on a Linux-shaped task. In the
Ubuntu container, `<cstdio>` just works. One environment bug can be the right moment
to question the whole environment.

**Generalizes to.** "Standard header not found" = toolchain/SDK path problem, not
your code. And when the host keeps throwing OS-specific walls at an OS-specific
task, containerizing is cheaper than a pile of per-wall workarounds.

---

## B3 🐛 Shell: `bash: handler: command not found` inside a diagnostic

**Symptom.** A `docker run bash -c "...."` diagnostic printed
`bash: line 8: handler: command not found`, and one `echo` came out with a word
missing.

**Root cause.** My own mistake, not the program's. Inside a **double-quoted**
`bash -c "..."`, backtick-wrapped words like `` `handler` `` are command
substitution — bash tried to *run* `handler` as a program. The surrounding heredoc
was double-quoted, so the backticks were live.

**Fix.** Cosmetic (the actual diagnostic data was unaffected), so noted rather than
chased: use single quotes, or escape the backticks, when a shell string contains
literal backticks.

**Generalizes to.** Quoting context is a real semantic boundary in shell. This one
was harmless; the same reflex ("it's just text") is how `rm`-adjacent one-liners go
wrong. Logged because the brief was *every* bug, including my own trivial ones.

---

## B4 🔬 `dladdr()` cannot see `static` functions — the design-breaking discovery

This is the most important entry: a spike invalidated the naive architecture
*before* it was written, which is the entire reason spikes came first.

**What I believed.** The obvious symbolizer is 10 lines: `backtrace()` to get PCs,
`dladdr()` to turn each PC into a function name. Ship it.

**The test that broke it.** `scratch/spike1.c` deliberately contains a `static`
(internal-linkage) function and profiles itself. Compiled with `-rdynamic` — the
case *most favourable* to `dladdr`, since it exports globals into `.dynsym`. If it
still fails there, the failure is decisive.

**Symptom.** The captured stack's innermost frame, `0xaaaae6770d84`, came back
`<UNRESOLVED>` from `dladdr`.

**How I isolated it.** `readelf --syms` on the same binary showed:
```
addr=0x0d58  size=64  bind=LOCAL  handler
```
`handler` spans `[0xd58, 0xd98)`; the unresolved PC minus the load bias is `0xd84`,
which lies inside it. So the unresolved frame *is* `handler` — and it's `bind=LOCAL`.

**Root cause.** `dladdr` resolves addresses only against the **dynamic** symbol
table `.dynsym`, which by definition holds no `LOCAL` symbols. `-rdynamic` promotes
*globals* into `.dynsym`; it never promotes internal-linkage symbols. Since real hot
code is routinely `static` translation-unit-private helpers, a `dladdr`-based
symbolizer prints `<UNRESOLVED>` for exactly the functions the user most needs named.

**Fix / architectural consequence.** The symbolizer must parse the full ELF
`.symtab` itself (SPEC §3.4): mmap the ELF, walk section headers to `SHT_SYMTAB`,
keep every `STT_FUNC` with `st_size>0` as a `[value, value+size)` range, subtract the
PIE load bias, and binary-search. Using `st_size` (vs `dladdr`'s nearest-preceding
guess) also means an address in a gap resolves to *nothing* instead of being
misattributed — correct-and-unknown beats confidently-wrong.

**Generalizes to.** Know which table an API reads before trusting it: `.dynsym`
(dynamic linking) vs `.symtab` (full, stripped in release builds). And: spike your
riskiest assumption first — this one-hour test saved a rewrite at task 6.

---

## B5 🔬 `-O1` inlined the hot code into `main` — sampling saw neither function

**Symptom.** Same spike. Both the `static` helper *and* the extern wrapper that
does the actual arithmetic were **absent from the sampled stacks**; the interrupted
frame was `main` itself. Worse, `hot_static_function` was missing from `.symtab`
**entirely**.

**Hypotheses.**
- (H1) The unwinder is dropping frames. — Checked: the frames present (`main`,
  `__libc_start_main`, `_start`) form a valid chain with no gap. Nothing is being
  lost mechanically.
- (H2) The functions were **inlined** and genuinely don't exist as frames at
  runtime. — Confirmed.

**How I isolated it.** `readelf --syms | grep hot_static` → absent (inlined away,
so no symbol emitted). `readelf --debug-dump=info | grep -c DW_TAG_inlined_subroutine`
→ **11**. So the compiler *did* inline, and DWARF *did* record it — the runtime call
stack simply no longer contains those frames.

**Root cause.** This isn't a bug to fix; it's the fundamental tension of the whole
project. You must profile **optimized** code (profiling `-O0` measures a program you
will never ship), but optimization **erases the very structure** you want to
attribute time to. A profiler that reports "100% in `main`" is technically true and
useless.

**Fix / decision (SPEC §8.1).** v1 attributes to the *physical* function present at
runtime, **detects** the distortion (a large `st_size` whose DWARF shows inlined
callees is suspect), and **warns** rather than silently misattributing. Recovering
the logical inline stack via `DW_TAG_inlined_subroutine` is real work (what `perf`
and VTune do) and is scoped as v2. A profiler that *knows what it cannot see* is
worth more than one that quietly lies.

**Generalizes to.** Optimization vs observability is a permanent trade in tooling
(it's also why debug info exists). This same mechanism — sampled profiles fed back
to guide inlining decisions — is exactly what LLVM's AutoFDO / SamplePGO do.

---

## B6 🐛 `std::abort` is not a member of `std` — missing `<cstdlib>`

**Symptom.** First real compile of the ring-buffer tests:
```
error: 'abort' is not a member of 'std'
   63 |       std::abort();
```

**Root cause.** In the power-of-two capacity guard I call `std::abort()`, but
`ring_buffer.h` only included `<atomic>`, `<cstddef>`, `<cstdint>`, `<memory>` —
none of which is guaranteed to declare `std::abort`. It "looked fine" because I
wrote the call and the includes at different moments and never reconciled them; on
another stdlib it might have compiled by transitive luck, which is worse.

**Fix.** Add `#include <cstdlib>` with a comment naming *why* (the abort in the
guard). A header must include what it uses — never lean on transitive includes,
because the day they change, the failure lands in someone else's translation unit.

**Generalizes to.** IWYU — "include what you use." Self-contained headers are a
correctness property, not a style nicety: a header that only compiles because of
what happened to be included before it is a latent break.

---

## B7 🐛 `Config cfg = {}` won't compile — and my first diagnosis was wrong

This one is kept in full *because* I guessed wrong first, then let the compiler
correct me. That sequence is the point.

**Symptom.** Building the sampler:
```
error: could not convert '<brace-enclosed initializer list>()' from
       '<brace-enclosed initializer list>' to 'hotpath::Sampler::Config'
   explicit Sampler(Config cfg = {});
```

**Hypothesis 1 (WRONG).** "A `= {}` default argument on an aggregate that has
default *member* initializers is the problem; value-initialize with `= Config()`
instead." I even wrote that into a code comment. It was plausible and it was
wrong.

**How it was disproven.** Two moves.
1. Changed `= {}` to `= Config()` and rebuilt → a *different, clearer* error:
   `default member initializer for 'Config::period_us' required before the end of
   its enclosing class`. So value-init didn't fix it — meaning `{}` vs `Config()`
   was never the axis. The compiler renamed the real problem for me.
2. Minimal repro isolating the true variable — nesting:
   ```cpp
   struct Free { unsigned x = 1; };
   struct UsesFree { explicit UsesFree(Free f = {}); };   // COMPILES
   struct Outer {
     struct Cfg { unsigned x = 1; };
     explicit Outer(Cfg c = {});                          // FAILS, same error
   };
   ```
   Non-nested aggregate with `= {}`: fine. Nested aggregate with `= {}`: the exact
   original error. So the cause is **nesting**, not braces.

**Root cause.** `Config` is nested in `Sampler`. A default argument is parsed in
the "complete-class context" of `Sampler`, but evaluating `Config{}` / `Config()`
as that default argument needs `Config`'s default *member* initializers — and
those are not available until `Sampler` (the enclosing class) is itself complete.
The standard specifically does not let a nested class's DMIs be used in a default
argument of the enclosing class's member function. GCC's first message just
happened to describe the *symptom* (`{}` conversion) instead of the *rule*.

**Fix.** Drop the default argument. Declare two constructors and delegate:
```cpp
// header
Sampler();                     // default Config
explicit Sampler(Config cfg);
// cpp -- here BOTH classes are complete, so Config{} is legal
Sampler::Sampler() : Sampler(Config{}) {}
```

**Generalizes to.** (1) When a fix produces a *different* error rather than
success, the new error is data — it often means your model of the bug was wrong,
not that you need another patch on the same theory. (2) Compiler messages name the
symptom, not always the rule; a 6-line minimal repro that toggles one variable
(here: nested vs not) finds the rule. (3) I had committed the wrong explanation to
a comment — worth remembering that a confident-sounding note can be exactly the
misinformation this journal exists to prevent, which is why it got corrected here
rather than left to rot.

---

## B8 🔬 The profiler skipped a middle frame — disassembly says exactly why

The Task-4 integration test recovered a known chain `main → hp_top → hp_mid →
hp_leaf`, but the middle function **hp_mid never appeared** in any of ~500 samples,
while its caller hp_top appeared in ~99%. A profiler silently dropping a frame is
alarming, so this got run to ground rather than waved off.

**Symptom.** `hp_leaf: self=498 incl=498 | hp_mid incl=0 | hp_top incl=498`.
hp_leaf (correct, that's the busy loop) and hp_top present; hp_mid absent.

**Ruling out the boring explanations.**
- *dladdr couldn't name hp_mid?* No — hp_mid is `extern "C"`, in `.dynsym`, and
  dladdr resolves hp_top which is adjacent. `incl=0` means no captured PC lands in
  hp_mid's address range at all. It is genuinely not on the stack.
- *Inlined or tail-called away?* Built with `-fno-optimize-sibling-calls`, and
  hp_mid does work after its call (`return hp_leaf(..) + 1.0`), so it is a real,
  non-tail call. Ruled out by disassembly below.

**The evidence — prologues (`objdump -d`).**
```
hp_leaf:  sub sp, sp, #0x10        <- allocates locals, but NO `stp x29,x30`
          str xzr, [sp, #8]           => hp_leaf never builds a frame record
hp_mid:   stp x29, x30, [sp,#-16]!  <- builds a frame record
          mov x29, sp
          bl  hp_leaf
hp_top:   stp x29, x30, [sp,#-16]!  <- builds a frame record
          bl  hp_mid
```

**Root cause — a precise instance of SPEC §8.2.** hp_leaf is a **frameless leaf**:
even under `-fno-omit-frame-pointer`, GCC omits the frame record for a function
that calls nothing. So when SIGPROF lands in hp_leaf's loop:
- `pc` is in hp_leaf → we record it as frame 0 (correct — seeding frame 0 from the
  interrupted pc is exactly what saves the leaf itself);
- `x29` has **not** been changed by hp_leaf, so it still points at **hp_mid's**
  frame record;
- the return address *into hp_mid* is sitting in the live **x30 (LR)** register,
  which v1 deliberately does not consult.

Walking from x29 = hp_mid's frame, the first saved return address we read is
hp_mid's own saved LR = the return **into hp_top** (hp_top is who called hp_mid).
So the walk yields `[hp_leaf, hp_top, main, …]` and **hp_mid falls in the gap**.
The casualty of a frameless leaf is not the leaf — it's the leaf's *immediate
caller*.

**Fix / decision.** None in v1, on purpose. The tempting fix — always push x30 as
frame 1 — would **double-count** the caller in the common case where the innermost
function *did* build a frame (then x30 is stale or duplicates frame 1). Emitting a
wrong frame is worse than omitting one (same principle as B5). Correctly deciding
"is the innermost function frameless / mid-prologue?" needs instruction inspection
or CFI, which is the v2 unwinder.

**A follow-on mistake, kept because it taught the real lesson.** I first "pinned"
the skip as a test assertion `CHECK_EQ(incl_count["hp_mid"], 0)`, reasoning it made
the limitation a tracked behavior. Then I built the same test at `-O1` under ASan
and it **failed**: hp_mid was suddenly present (`incl=496`). Disassembling the ASan
build showed hp_leaf now *opens* with `stp x29,x30,[sp,#-112]!` — ASan
instrumentation (and lower optimization) gives the leaf a real frame, so its caller
is no longer skipped. The skip is therefore an artifact of the **target's** `-O2`
frameless-leaf optimization, **not an invariant of the sampler**. Pinning it
asserted the wrong thing: a property of GCC's codegen dressed up as a property of
my code. Fixed by **reporting** the skip (and explaining it) instead of asserting
it; the robust assertions (leaf dominates self-time; the chain is unwound past the
leaf to hp_top) hold under both builds.

**Generalizes to.** (1) Frame-pointer unwinding has a structural blind spot at
frameless leaves — *the* reason perf/VTune reach for CFI/`.eh_frame`/LBR. (2) What
a profiler sees is a function of how the **target** was compiled; the same code
profiled at `-O1` vs `-O2` yields different stacks. (3) A test must assert *your*
code's contract, not the toolchain's incidental choices — and running the suite
under a second build (ASan/`-O1`) is what exposed that I'd blurred the two.

**Generalizes to.** Frame-pointer unwinding has a structural blind spot at
frameless leaves; this is *the* reason production profilers (perf, VTune) reach for
CFI/`.eh_frame` or LBR. Knowing precisely which frame is lost — the leaf's caller,
because the link is still in LR — is the difference between "my profiler is buggy"
and "my profiler has a characterized, tested limitation with a known fix path."

---

## Cross-cutting notes

- **Spikes before architecture.** B4 and B5 both came from one 40-line disposable
  program run before any real code. Two of the three load-bearing design decisions
  (parse `.symtab`; treat inlining as a first-class distortion) are direct results.
- **Report drops, don't hide them.** The ring buffer counts dropped samples rather
  than blocking (which would deadlock) or growing (which would `malloc` in a signal
  handler). A profiler that silently discards data is misreporting its own coverage.
- **Prove concurrency, don't assert it.** The SPSC buffer's memory ordering is
  verified under ThreadSanitizer (200k cross-thread ops, zero races), not argued
  from a lucky run.
