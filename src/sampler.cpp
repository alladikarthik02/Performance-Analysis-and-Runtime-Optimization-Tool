// hotpath — sampler implementation. See sampler.h for the design contract.
// (g++ auto-defines _GNU_SOURCE, which pthread_getattr_np needs; no manual
// #define -- doing so just warns about redefinition.)
#include "hotpath/sampler.h"

#include <dirent.h>
#include <pthread.h>
#include <sys/time.h>
#include <ucontext.h>

#include <atomic>
#include <cstring>

namespace hotpath {
namespace {

// The one piece of global state the handler can reach. Set in start(), cleared
// in stop(). acquire/release so a handler that observes a non-null pointer also
// observes the fully-constructed object it points at.
std::atomic<Sampler*> g_active{nullptr};

// Count OS threads by listing /proc/self/task. opendir/readdir allocate, so this
// is called from start() (normal context), never the handler.
int count_threads() {
  DIR* d = opendir("/proc/self/task");
  if (!d) return -1;
  int n = 0;
  for (dirent* e; (e = readdir(d)) != nullptr;) {
    if (e->d_name[0] != '.') ++n;  // skip "." and ".."
  }
  closedir(d);
  return n;
}

// The current thread's stack extent, via glibc's non-portable getattr_np. This
// mallocs internally (it consults /proc/self/maps for the main thread), which is
// exactly why it must run in start() and be cached -- never in the handler.
bool query_stack_bounds(StackBounds* out) {
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) != 0) return false;
  void* addr = nullptr;
  std::size_t size = 0;
  const int rc = pthread_attr_getstack(&attr, &addr, &size);
  pthread_attr_destroy(&attr);
  if (rc != 0 || addr == nullptr) return false;
  out->low = reinterpret_cast<std::uintptr_t>(addr);
  out->high = reinterpret_cast<std::uintptr_t>(addr) + size;
  return true;
}

}  // namespace

// Delegating default ctor. Config{} is legal HERE (unlike as a default argument
// in the header) because both Sampler and Config are complete at this point.
Sampler::Sampler() : Sampler(Config{}) {}

Sampler::Sampler(Config cfg) : cfg_(cfg), ring_(cfg.buffer_capacity) {}

Sampler::~Sampler() {
  if (running_) stop();
}

void Sampler::handler(int /*sig*/, siginfo_t* /*si*/, void* ucontext) {
  // Recover state. If we lost the race with stop() (pointer already cleared),
  // there is nothing valid to write into -- bail. This is the only correct thing
  // to do for a possibly-in-flight tick.
  Sampler* self = g_active.load(std::memory_order_acquire);
  if (self == nullptr) return;

  auto* uc = static_cast<ucontext_t*>(ucontext);
  // Verified field paths (spike): pc, and x29/x30 in regs[]. These are plain
  // reads of the interrupted machine state -- no allocation, no locks.
  const std::uintptr_t pc = static_cast<std::uintptr_t>(uc->uc_mcontext.pc);
  const std::uintptr_t fp = static_cast<std::uintptr_t>(uc->uc_mcontext.regs[29]);

  std::uint64_t frames[kMaxFrames];
  const std::size_t n = unwind_fp(pc, fp, self->bounds_, frames, kMaxFrames);

  // Drop-and-count on overflow (never block, never grow); accounting lives in
  // the ring buffer. Signal-safe end to end.
  self->ring_.try_push(frames, static_cast<std::uint16_t>(n));
}

void Sampler::start() {
  if (running_) return;

  // --- everything expensive happens here, once, in normal context ---
  if (!query_stack_bounds(&bounds_)) {
    warnings_ += "could not determine stack bounds; unwinding disabled beyond pc\n";
    bounds_ = StackBounds{0, 0};  // walkable_fp() will reject all -> only pc captured
  }

  const int nthreads = count_threads();
  if (nthreads > 1) {
    // SPEC 8.3: process-wide SIGPROF lands on an arbitrary thread. We refuse to
    // pretend per-thread attribution is meaningful. Warn loudly; still sample.
    warnings_ += "target is multi-threaded (" + std::to_string(nthreads) +
                 " threads); ITIMER_PROF attribution is unreliable (SPEC 8.3)\n";
  }

  g_active.store(this, std::memory_order_release);

  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = &Sampler::handler;
  // SA_SIGINFO: we need the ucontext (3rd arg) for pc/fp. SA_RESTART: don't make
  // the profiled program observe EINTR on its syscalls because we sampled it.
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGPROF, &sa, &old_sa_) != 0) {
    warnings_ += "sigaction(SIGPROF) failed; no samples will be collected\n";
    g_active.store(nullptr, std::memory_order_release);
    return;
  }

  struct itimerval t;
  t.it_interval.tv_sec = cfg_.period_us / 1000000;
  t.it_interval.tv_usec = cfg_.period_us % 1000000;
  t.it_value = t.it_interval;  // first tick one period from now
  if (setitimer(ITIMER_PROF, &t, nullptr) != 0) {
    warnings_ += "setitimer(ITIMER_PROF) failed; no samples will be collected\n";
    sigaction(SIGPROF, &old_sa_, nullptr);
    g_active.store(nullptr, std::memory_order_release);
    return;
  }
  running_ = true;
}

void Sampler::stop() {
  if (!running_) return;

  // Order matters. Disarm the timer FIRST so no new ticks arrive, then detach
  // the global pointer so any in-flight handler bails, then restore the old
  // handler. A tick that already loaded a non-null `self` used state that is
  // still alive (we are inside stop(), object not yet destroyed).
  struct itimerval off;
  std::memset(&off, 0, sizeof(off));
  setitimer(ITIMER_PROF, &off, nullptr);
  g_active.store(nullptr, std::memory_order_release);
  sigaction(SIGPROF, &old_sa_, nullptr);
  running_ = false;

  // Drain in normal context -- allocation is fine here.
  samples_.clear();
  samples_.reserve(ring_.size());
  Sample s;
  while (ring_.try_pop(s)) samples_.push_back(s);
}

}  // namespace hotpath
