# hotpath — Challenge Log

These are the **real** engineering problems we hit while building this profiler,
reconstructed only from what actually happened during the build — the commands that
failed, the exact error messages, and the fixes we applied. Nothing here is a
"typical problem" added for completeness; if it's listed, it happened. Everything is
explained in plain English, with jargon defined the first time it appears. A few
challenges are marked **Deferred** — the cause is understood and the fix is designed,
but the code that fixes them (the symbolizer, the v2 unwinder) hasn't been built yet,
and this log says so honestly rather than pretending they're closed.

---

## CHAL-001 — macOS couldn't find a standard C++ header

- **Date:** 2026-07-15 · **Phase:** Environment setup (first spike) · **Status:** Resolved (by moving to Docker)

### What we were doing
Writing a tiny throwaway test program (a "spike") to check whether the core profiling
primitives even work, and compiling it on the Mac directly with `clang++`.

### Background
- A **compiler** turns C++ source into a runnable program. On a Mac the compiler is
  `clang++`.
- A **standard header** like `<cstdio>` is a file the compiler must find to know what
  standard functions (like `printf`) look like. The compiler searches a fixed list of
  folders for these.
- An **SDK** (Software Development Kit) is the bundle of headers and libraries that
  ship with the OS toolchain. On macOS it lives under a path you can ask for with
  `xcrun --show-sdk-path`.

### What went wrong
The compile died immediately with:
```
fatal error: 'cstdio' file not found
```
This is alarming: `<cstdio>` is one of the most basic C++ headers. If *that* can't be
found, nothing will compile.

### Why
It wasn't our code — it was the toolchain's search path. This machine has a **partial
Command Line Tools install**: the folder the compiler looks in *by default* for the
C++ standard library headers is a broken stub (nearly empty), while the *real* headers
live under the SDK folder. So the compiler looked in the wrong place and found nothing.
Analogy: the library catalog points at an empty shelf, even though the books are in the
back room. (We'd actually seen this exact issue before — an earlier project's build
file documented the same workaround.)

### Fix
The direct fix is to point the compiler at the SDK explicitly with the `-isysroot`
flag. But we never needed to: this error was one of several signs (no `perf`, macOS
uses a different binary format than Linux) that the Mac was the wrong place to build a
Linux-shaped profiler. We **moved the whole project into a Docker container** running
Ubuntu Linux, where the headers are exactly where the compiler expects. The error
simply cannot occur there.

### Takeaway
A missing *standard* header almost always means a broken toolchain path, not broken
code — and when the host keeps fighting an OS-specific task, containerizing beats a pile
of one-off workarounds.

---

## CHAL-002 — Docker wouldn't respond ("is the daemon running?")

- **Date:** 2026-07-15 · **Phase:** Environment setup · **Status:** Resolved

### What we were doing
Switching the build into Docker, and running our first `docker` command to check the
environment.

### Background
**Docker** is really two programs: a small **client** (the `docker` command you type)
and a background **daemon** (also called the "engine") that does the actual work of
building and running containers. The client just sends requests to the daemon over a
local socket (a **socket** is a file-like channel two programs use to talk). If the
daemon isn't running, the client has no one to talk to.

### What went wrong
```
failed to connect to the docker API at unix:///Users/.../docker.sock;
check if the path is correct and if the daemon is running:
... connect: no such file or directory
```

### Why
Docker Desktop (the app that hosts the daemon) simply wasn't started yet. The client
tried to reach the daemon through its socket, but nothing was listening — like calling
a phone number that's switched off.

### Fix
Launch the app (`open -a Docker`) and wait a few seconds for the daemon to come up,
then re-run the command. (This recurs any time Docker Desktop is quit — for example, it
was down again at the start of the next day's session and needed the same restart.)

### Takeaway
"Cannot connect to the daemon" means the background engine isn't running — the Docker
command you type and the engine that does the work are two separate programs.

---

## CHAL-003 — `docker build` failed looking for a credential helper

- **Date:** 2026-07-15 · **Phase:** Environment setup · **Status:** Resolved

### What we were doing
Building our Docker image for the first time (the image is the pre-baked Linux
environment with the compiler and tools installed).

### Background
- **PATH** is an environment variable holding the list of folders the shell searches
  when you run a command by name. If a program isn't in one of those folders, the
  shell reports "not found" even when the program exists elsewhere on disk.
- A **credential helper** is a small companion program Docker calls to fetch login
  credentials for image registries. Docker's config file names which helper to use.

### What went wrong
The build died before running a single build step:
```
error getting credentials - err: exec: "docker-credential-desktop":
executable file not found in $PATH
```

### Why
Two facts combined. (1) Docker's global config (`~/.docker/config.json`) said
`"credsStore": "desktop"`, which tells the Docker client to run a helper program named
`docker-credential-desktop` for registry logins. (2) That helper program **does exist**,
but it's tucked inside the Docker Desktop application bundle
(`/Applications/Docker.app/Contents/Resources/bin`) — a folder that is **not on the
PATH** of a plain non-interactive shell. So Docker tried to run a helper the shell
couldn't locate. Analogy: the instructions say "call the specialist," but the
specialist's phone number is written on a card locked in a drawer the shell can't open.

### Fix
Prepend the app-bundle folder to PATH **only for our own commands** — baked into our
`scripts/dev.sh` wrapper. We deliberately did **not** edit the global
`~/.docker/config.json` to remove `credsStore`, because that file governs every other
project on the machine; changing it to fix our local PATH gap would be a global change
for a local problem. (We only pull public images, so no credentials are actually
needed anyway.)

### Takeaway
"Executable not found" for a *helper* is a PATH problem, not a missing install — and fix
it in the narrowest scope you can, never by mutating shared global config.

---

## CHAL-004 — The obvious way to get function names was blind to half our functions

- **Date:** 2026-07-15 · **Phase:** Spikes (de-risking the design) · **Status:** Deferred (real fix — an ELF symbol-table parser — is designed but not yet built)

### What we were doing
Testing the assumption at the heart of the profiler: after we capture a raw memory
address of running code, can we turn it into a human-readable **function name**? The
"obvious" tool for this is a standard library function called `dladdr`.

### Background
- When code runs, "where are we?" is just a **memory address** — a number like
  `0x...d84`. To be useful, a profiler must map that number back to a name like
  `hp_leaf`. That mapping step is called **symbolization**.
- A compiled program carries **symbol tables**: lists that say "the function named X
  lives at address Y." There are two such tables. The **dynamic symbol table**
  (`.dynsym`) lists only symbols meant to be visible to *other* programs at load time.
  The **full symbol table** (`.symtab`) lists *everything*, including internal
  functions.
- In C/C++, a function marked `static` has **internal linkage** — it's private to its
  own source file and deliberately *not* exported. Such functions go in `.symtab` but
  **not** in `.dynsym`.
- `dladdr` is the easy, 2-line symbolizer — but it reads **only `.dynsym`**.

### What went wrong
Our spike deliberately included a `static` helper function and asked `dladdr` to name
the address where the profiler interrupted it. `dladdr` returned:
```
[ 0] 0xaaaae6770d84   <UNRESOLVED>
```
It couldn't name it — even though we compiled with the most *favorable* settings for
`dladdr` (a flag called `-rdynamic` that exports as much as possible).

### Why
The unresolved function was `static`, so it lived only in `.symtab`, and `dladdr` only
ever looks in `.dynsym`. We proved this with the standard `readelf` tool, which showed
the function marked `bind=LOCAL` (internal). Since real programs put their hottest,
performance-critical code in `static` helpers all the time, a `dladdr`-based
symbolizer would print `<UNRESOLVED>` for **exactly the functions the user most needs
named**. Analogy: `dladdr` reads only the "public directory," but the busiest workers
are unlisted staff — invisible to it.

### Fix
**Designed, not yet built.** The symbolizer must parse the full `.symtab` ourselves:
read the program file, find the symbol table, and keep every function as an address
range `[start, start+size)` so any captured address can be looked up. This is scheduled
as a later task (the symbolizer), so as of now the tool captures raw addresses but does
**not** yet name them. Marked Deferred honestly.

### Takeaway
Know *which table* your API reads: `dladdr` sees only the dynamic symbol table, so it's
blind to every `static` (internal) function — often the ones you most want named.

<details><summary>Raw evidence</summary>

```
dladdr saw hot_static_function: NO
VERDICT: dladdr is INSUFFICIENT -> must parse ELF .symtab ourselves
--- .dynsym (what dladdr can see) --- : only hot_extern_function
--- .symtab (what a real parser sees) --- : handler bind=LOCAL, etc.
```
</details>

---

## CHAL-005 — Our hottest functions vanished from the call stack

- **Date:** 2026-07-15 · **Phase:** Spikes (de-risking the design) · **Status:** Deferred (inline-aware attribution is a later task)

### What we were doing
In the same spike, checking whether the captured **call stack** (the chain of "who
called whom" leading to the current spot) actually contained the functions doing the
work.

### Background
- A **call stack** is the breadcrumb trail of active function calls: `main` called
  `hp_top` called `hp_leaf`, etc.
- **Inlining** is a compiler optimization: instead of *calling* a small function, the
  compiler copies its body directly into the caller. This is faster (no call overhead)
  but it means the inlined function no longer exists as a separate entry on the stack —
  its code is now *part of* the caller.

### What went wrong
The spike had a busy function doing real arithmetic, called through a wrapper. But the
captured stack contained **neither** — the innermost real frame was `main` itself. And
when we checked the symbol table, the busy function was **absent entirely**:
```
hot_static_function ABSENT from .symtab -> inlined away
```

### Why
The compiler **inlined** both the busy function and its wrapper straight into `main`.
Their machine code became part of `main`, so at profiling time there was no separate
`hot_function` frame to see — the profiler correctly reported `main`, because that is
genuinely the function the CPU was executing. We confirmed the compiler recorded the
inlining in the debug info (`DW_TAG_inlined_subroutine` appeared 11 times). This is the
deepest tension in any profiler: **you must profile *optimized* code** (profiling the
slow unoptimized build tells you about a program you'll never ship), **but optimization
erases the very call structure you want to attribute time to.**

### Fix
**Deferred, with an honest v1 stance.** A full fix means reading the compiler's debug
info to reconstruct the "logical" inlined call chain — real, and expensive (it's what
`perf` and VTune do). For version 1, the plan is to attribute time to the *physical*
function that actually exists at runtime, **detect** when inlining likely distorted the
picture, and **warn** — rather than silently misreport. That detection/attribution code
is a later task, so this is Deferred.

### Takeaway
You must profile optimized code, but optimization erases structure — inlining can make
your hottest function disappear from the stack entirely, folded into its caller.

---

## CHAL-006 — The shell ran our text as a command (backticks)

- **Date:** 2026-07-15 · **Phase:** Spikes · **Status:** Resolved

### What we were doing
Running a diagnostic command inside the container to confirm the CHAL-004 finding,
where our shell text mentioned a function called `handler` wrapped in backticks.

### Background
In a Unix shell, **backticks** `` `like this` `` are not decoration — they mean
"run whatever is inside as a command and paste its output here" (this is called
**command substitution**). Whether backticks are "live" depends on the surrounding
**quoting**: inside *double* quotes they still execute; inside *single* quotes they're
literal text.

### What went wrong
```
bash: line 2: handler: command not found
bash: line 8: handler: command not found
```
One line of our diagnostic printed with a word missing.

### Why
Our diagnostic string was inside double quotes, and it contained `` `handler` ``. The
shell obediently tried to *run a program called `handler`* and substitute its output —
but no such program exists, so it errored and left a blank. The actual data we were
inspecting was fine; only the label text was mangled. Analogy: you wrote a note that
said "call `handler`," and the assistant literally dialed a contact named handler
instead of reading the word.

### Fix
Cosmetic, so noted rather than chased down mid-investigation: use single quotes (or
escape the backticks) when a shell string contains literal backticks. (This same class
of quoting mistake bit us again later — see CHAL-012.)

### Takeaway
In a double-quoted shell string, backticks **execute** — quoting context is real syntax,
not decoration.

---

## CHAL-007 — `std::abort` didn't compile (missing include)

- **Date:** 2026-07-16 · **Task:** Ring buffer (Task 2) · **Status:** Resolved

### What we were doing
First compile of the ring-buffer's tests. The ring buffer has a safety check that calls
`std::abort()` (an immediate, deliberate crash) if it's misconfigured.

### Background
- In C++ you must `#include` the header that declares any standard function you call,
  so the compiler knows it exists. `std::abort` is declared in `<cstdlib>`.
- Headers sometimes pull in *other* headers indirectly, so code can occasionally
  compile "by accident" even with a missing include — until the day it doesn't.

### What went wrong
```
error: 'abort' is not a member of 'std'
```

### Why
The ring-buffer header called `std::abort()` but only included `<atomic>`, `<cstddef>`,
`<cstdint>`, and `<memory>` — none of which is guaranteed to declare `std::abort`. I
wrote the call and the include list at different moments and never reconciled them.

### Fix
Add `#include <cstdlib>` to the header. The rule this enforces is "**include what you
use**": a header should directly include every header for the names it references,
rather than relying on another include happening to drag it in — because the day that
transitive path changes, the break lands in someone else's file.

### Takeaway
A header must include what it uses; code that compiles only by luck of transitive
includes is a latent break waiting for a rainy day.

---

## CHAL-008 — A harmless-but-noisy `_GNU_SOURCE` redefinition warning

- **Date:** 2026-07-16 · **Task:** Sampler (Task 4) · **Status:** Resolved

### What we were doing
Compiling the sampler code, which uses a Linux-specific function (`pthread_getattr_np`)
that requires a special switch to be turned on.

### Background
`_GNU_SOURCE` is a **feature-test macro**: defining it before including system headers
tells them "expose the GNU/Linux extensions, not just the portable subset." Some
functions are hidden unless it's defined.

### What went wrong
```
warning: "_GNU_SOURCE" redefined
```

### Why
I manually wrote `#define _GNU_SOURCE` at the top of the file — but the C++ compiler
driver (`g++`) **already defines it for you automatically**. So it was defined twice,
and the compiler warned about the redundant redefinition. (Not an error — the build
still worked — but noise that hides real warnings.)

### Fix
Delete the manual `#define`. It was needed in the earlier C spikes (the C driver does
*not* auto-define it), but not in the C++ files.

### Takeaway
The C++ compiler already defines `_GNU_SOURCE` for you — redefining it just adds noise.

---

## CHAL-009 — A default value wouldn't compile, and my first diagnosis was wrong

- **Date:** 2026-07-16 · **Task:** Sampler (Task 4) · **Status:** Resolved

### What we were doing
Giving the `Sampler` class a constructor with a **default configuration**, so callers
could write `Sampler()` and get sensible defaults. The config is a small nested struct
whose fields have built-in default values.

### Background
- A **constructor** is the function that builds an object. A **default argument** lets a
  parameter be omitted (`Sampler(Config cfg = {})` means "if you don't pass a config,
  use an empty one").
- A **nested struct** is a struct defined *inside* another class (`Config` inside
  `Sampler`). A class is **incomplete** until the compiler reaches its closing brace —
  and there are things you're not allowed to do until it's complete.

### What went wrong
```
error: could not convert '<brace-enclosed initializer list>()' from
       '<brace-enclosed initializer list>' to 'hotpath::Sampler::Config'
```
at `Sampler(Config cfg = {})`.

### Why (and the wrong turn)
My **first** hypothesis was that the empty-braces `{}` default value was the problem, so
I changed it to `= Config()`. That produced a **different, clearer** error:
```
error: default member initializer for 'Config::period_us'
       required before the end of its enclosing class
```
The new error was the real clue: the problem was never the braces — it was **nesting**.
I confirmed it with a 6-line minimal test that toggled exactly one variable:

- A non-nested struct with a `= {}` default argument → **compiles fine.**
- The *same* struct nested inside another class → **fails with the original error.**

The real rule: to build a default `Config` value *inside* `Sampler`'s own declaration,
the compiler needs `Config`'s built-in field defaults — but those aren't available yet,
because `Sampler` (the enclosing class) isn't finished being defined at that point.
You're asking to use a room's furniture before the room's walls are up.

### Fix
Stop using a default argument. Declare two constructors instead — `Sampler()` and
`Sampler(Config)` — and have the no-arg one **delegate** to the other from inside the
`.cpp` file, where both classes are fully complete and building a `Config{}` is legal.

### Takeaway
When a fix produces a *different* error instead of success, your model of the bug was
wrong — a tiny minimal repro that toggles one variable finds the real rule faster than
guessing.

---

## CHAL-010 — The profiler silently skipped a middle function in the call stack

- **Date:** 2026-07-16 · **Task:** Sampler integration (Task 4) · **Status:** Deferred (characterized limitation; the real fix is a v2 unwinder)

### What we were doing
The first real end-to-end test: run the profiler on a program with a *known* call chain
— `main → hp_top → hp_mid → hp_leaf` — and check that the captured stacks match.

### Background
- On ARM chips, the profiler figures out the call stack by following **frame pointers**:
  each function, on entry, saves a little record on the stack (its caller's frame
  pointer + the return address), and these records form a linked list the profiler walks
  upward. This save happens in the function's **prologue** (its first few instructions).
- A **leaf function** is one that calls nothing else. Compilers often skip building the
  frame-pointer record for a leaf, since nothing will walk *past* it — a small speed win.

### What went wrong
The test passed its main checks, but the diagnostics showed the **middle** function
missing from every one of ~500 samples:
```
hp_leaf: self=498 incl=498 | hp_mid incl=0 | hp_top incl=498
```
`hp_leaf` (innermost) and `hp_top` (outer) were seen; `hp_mid` (between them) appeared
**zero** times. A profiler silently dropping a frame is alarming, so we dug in instead
of shrugging.

### Why
We disassembled the three functions (looked at their actual machine instructions). The
smoking gun: `hp_leaf` was compiled as a **frameless leaf** — it bumped the stack for
its local variable but **never saved a frame-pointer record** (`sub sp,...` present, but
no `stp x29,x30`). Its two callers *did* save records.

So when the timer interrupted `hp_leaf`: the profiler correctly recorded `hp_leaf` from
the live instruction pointer, but then started walking frame records from `hp_mid`'s
record (because `hp_leaf` never made its own). `hp_mid`'s saved return address points to
**its** caller, `hp_top`. Result: `hp_leaf → hp_top`, and **`hp_mid` falls into the
gap**. The link from the leaf to its immediate caller lived only in a CPU register
(`x30`), which version 1 deliberately doesn't consult. The casualty of a frameless leaf
isn't the leaf — it's the leaf's *immediate caller*.

### Fix
**Intentionally none in v1.** The tempting fix — always trust that `x30` register as the
next frame — would **invent a wrong frame** in the common case where the innermost
function *did* save a record (then `x30` is stale). A wrong frame is worse than a
missing one. Correctly deciding "is this a frameless leaf?" needs deeper machinery
(reading unwind tables), which is the version-2 unwinder. For v1 we characterize and
report the limitation rather than paper over it. Status: Deferred.

### Takeaway
Frame-pointer unwinding has a blind spot — a leaf that builds no frame record makes its
own *caller* invisible, which is exactly why production profilers reach for unwind
tables or hardware call-stack tracing.

---

## CHAL-011 — A test "fix" quietly asserted a compiler quirk as if it were our rule

- **Date:** 2026-07-16 · **Task:** Sampler test hardening (Task 4) · **Status:** Resolved

### What we were doing
After understanding CHAL-010, I "pinned" the missing-frame behavior as a test assertion
(`the middle frame count must equal 0`), reasoning that this documented the limitation.
Then I re-ran the same test compiled under a different, stricter build.

### Background
- **AddressSanitizer (ASan)** is a build mode that instruments a program to catch
  invalid memory accesses. It's run at a lower optimization level and adds bookkeeping
  code around functions.
- **Optimization level** (`-O2` vs `-O1`) changes how aggressively the compiler
  rewrites code — including whether it bothers to give a leaf function a frame record.

### What went wrong
Under the ASan build the test **failed**:
```
CHECK_EQ(incl_count["hp_mid"], 0)  got: 496 vs 0
```
The middle frame I had just asserted was *always* missing… was now **present** in 496 of
498 samples.

### Why
At the higher optimization level (`-O2`), `hp_leaf` was a frameless leaf, so its caller
`hp_mid` was skipped (CHAL-010). But under ASan's lower-optimization, instrumented
build, the compiler **did** give `hp_leaf` a real frame record — we confirmed this by
disassembling the ASan binary and seeing the frame-save instruction
(`stp x29, x30, ...`) now present. With a frame, the walk no longer skips `hp_mid`, so
it reappears. My assertion had pinned an **artifact of one specific compiler
configuration** as if it were an invariant of *my* code. It isn't: whether a frame is
skipped depends on how the *target* was compiled, which is not my profiler's contract.

### Fix
Replace the hard assertion with a **report**: the test now prints whether the leaf-skip
happened and explains it, but only *asserts* the build-independent truths (the innermost
function dominates self-time; the stack is unwound up to the outer function). Those hold
under both builds. So the earlier "fix" (the pin) was itself the bug, and running a
second build configuration is what exposed it.

### Takeaway
Test *your code's* contract, not the compiler's incidental choices — running the suite
under a second build (like ASan) is what revealed I'd confused the two.

---

## CHAL-012 — Shell quoting broke an `awk` one-liner (the same lesson, twice)

- **Date:** 2026-07-16 · **Task:** Leaf-skip investigation (Task 4) · **Status:** Resolved

### What we were doing
While confirming CHAL-011, trying to disassemble a single function out of the binary
with a compact `awk` command run inside `docker ... bash -c "..."`.

### Background
- `awk` is a text-processing tool driven by a small script, and that script is normally
  wrapped in single quotes.
- The trouble is **nesting quotes**: our whole command was already inside the double
  quotes of `bash -c "..."`, and inside that we tried to use more quotes for the `awk`
  script. The shell parses quotes before `awk` ever sees the script, and mismatched
  layers confuse it.

### What went wrong
```
(eval):6: unmatched '
```
The command didn't run at all — the shell gave up parsing it.

### Why
A quoting-layer collision: with `awk`'s quotes sitting inside `bash -c`'s double quotes,
the shell saw an odd number of quote characters and reported an "unmatched" quote. This
is the *same class* of mistake as CHAL-006 (the backticks) — the shell's quoting is a
real grammar, and stacking quote types inside `bash -c` is fragile.

### Fix
Stop nesting quotes: use a tool flag that avoids the embedded script entirely —
`objdump --disassemble=hp_leaf` piped to `grep` — which needs no inner quotes and read
cleanly. (A more robust general habit for this environment: avoid clever quoted
one-liners inside `bash -c` and prefer flags or simpler pipelines.)

### Takeaway
Same lesson as the backticks, a second time: shell quoting is a real language — when a
quoted one-liner explodes, reach for a tool flag instead of nesting more quotes.

---

## Status summary

| ID | Title | Status |
|----|-------|--------|
| CHAL-001 | macOS missing `<cstdio>` | Resolved (moved to Docker) |
| CHAL-002 | Docker daemon not running | Resolved |
| CHAL-003 | `docker-credential-desktop` not on PATH | Resolved |
| CHAL-004 | `dladdr` blind to `static` functions | **Deferred** — symbolizer not built yet |
| CHAL-005 | Inlining erased hot functions from the stack | **Deferred** — inline-aware attribution not built |
| CHAL-006 | Backticks ran text as a command | Resolved |
| CHAL-007 | `std::abort` missing `<cstdlib>` | Resolved |
| CHAL-008 | `_GNU_SOURCE` redefined warning | Resolved |
| CHAL-009 | Nested-class default-argument won't compile | Resolved |
| CHAL-010 | Profiler skips a frameless leaf's caller | **Deferred** — v2 unwinder |
| CHAL-011 | Test pinned a compiler artifact; ASan exposed it | Resolved |
| CHAL-012 | Nested shell quoting broke `awk` | Resolved |
