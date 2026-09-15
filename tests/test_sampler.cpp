// Integration test: SIGPROF -> ucontext -> unwinder -> ring, against a REAL
// stack and a REAL timer. The fault-injection tests (test_unwind) proved the
// walker is safe on corrupt input; this proves it captures a correct call chain
// from a live program.
//
// Workload: a known nesting main -> hp_top -> hp_mid -> hp_leaf, with the busy
// loop in the leaf. We then assert the sampler recovers that structure:
//   * hp_leaf dominates SELF time (frame 0), because that is where the CPU is;
//   * hp_top appears INCLUSIVELY in most stacks, proving multi-frame unwinding.
//
// dladdr is used here only as a test oracle to name frames; it works because the
// workload functions are extern "C" + the test links -rdynamic. The real tool
// parses ELF .symtab (Task 6) precisely because dladdr cannot see static fns.
// (g++ auto-defines _GNU_SOURCE for dladdr; no manual #define needed.)
#include <dlfcn.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>

#include "hotpath/sampler.h"
#include "test.h"

using hotpath::Sample;
using hotpath::Sampler;

// -fno-optimize-sibling-calls (set in the build) keeps these as real nested
// frames instead of collapsing the tail calls into jumps. volatile keeps the
// loop from being optimized to nothing.
extern "C" __attribute__((noinline)) double hp_leaf(int iters) {
  volatile double acc = 0;
  for (int i = 1; i <= iters; ++i) acc += 1.0 / i;
  return acc;
}
extern "C" __attribute__((noinline)) double hp_mid(int iters) {
  double x = hp_leaf(iters);
  return x + 1.0;
}
extern "C" __attribute__((noinline)) double hp_top(int iters) {
  double x = hp_mid(iters);
  return x + 2.0;
}

// Name the function enclosing a PC, via dladdr (test oracle only).
static std::string name_of(std::uint64_t pc) {
  Dl_info info;
  if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_sname)
    return info.dli_sname;
  return "<unresolved>";
}

TEST(sampler_recovers_known_call_chain) {
  Sampler sampler(Sampler::Config{/*period_us=*/2000, /*capacity=*/1u << 15});
  sampler.start();

  // Burn ~1s of CPU in the known chain. At a 2 ms period that is ~500 ticks:
  // enough for the self/inclusive fractions to be statistically meaningful.
  volatile double sink = 0;
  auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t0)
             .count() < 1000) {
    sink += hp_top(4000);
  }
  (void)sink;

  sampler.stop();

  const auto& samples = sampler.samples();

  // Diagnostics -- printed so the run itself is legible, not just pass/fail.
  std::printf("  collected %zu samples, %llu dropped, %llu total ticks\n",
              samples.size(),
              static_cast<unsigned long long>(sampler.dropped()),
              static_cast<unsigned long long>(sampler.total_ticks()));
  if (!sampler.warnings().empty())
    std::printf("  warnings: %s", sampler.warnings().c_str());

  // 1) We actually sampled. On a ~1s CPU burn at 2 ms we expect hundreds; assert
  // a conservative floor so the test is not flaky on a slow/contended VM.
  CHECK(samples.size() >= 30);

  // 2) No spurious multi-thread warning (this process is single-threaded).
  CHECK(sampler.warnings().find("multi-threaded") == std::string::npos);

  // Tally self (frame 0) and inclusive (any frame) attribution by name.
  std::map<std::string, int> self_count, incl_count;
  int multiframe = 0, leaf_to_top_chain = 0;
  for (const auto& s : samples) {
    if (s.n_frames == 0) continue;
    if (s.n_frames >= 3) ++multiframe;
    self_count[name_of(s.pc[0])]++;

    bool seen_leaf = false, seen_top = false;
    std::string prev;
    for (std::uint16_t i = 0; i < s.n_frames; ++i) {
      std::string nm = name_of(s.pc[i]);
      if (nm != prev) incl_count[nm]++;  // count once per sample per name
      if (nm == "hp_leaf") seen_leaf = true;
      if (nm == "hp_top") seen_top = true;
      prev = nm;
    }
    if (seen_leaf && seen_top) ++leaf_to_top_chain;
  }

  // Actual self-time leader = the max, not map::begin() (which is alphabetical).
  std::string leader = "-";
  int leader_n = 0;
  for (const auto& kv : self_count)
    if (kv.second > leader_n) { leader = kv.first; leader_n = kv.second; }
  std::printf("  self-time leader: %s (%d)  multiframe samples: %d\n",
              leader.c_str(), leader_n, multiframe);
  std::printf("  hp_leaf: self=%d incl=%d | hp_mid incl=%d | hp_top incl=%d | full-chain=%d\n",
              self_count["hp_leaf"], incl_count["hp_leaf"], incl_count["hp_mid"],
              incl_count["hp_top"], leaf_to_top_chain);

  // 3) SELF time is dominated by the leaf -- that is where the CPU actually is.
  // Require a strong majority of frame-0 hits to be hp_leaf.
  CHECK(self_count["hp_leaf"] * 2 > static_cast<int>(samples.size()));

  // 4) Multi-frame unwinding works: hp_top is reached INCLUSIVELY in most
  // samples, i.e. we walked past the leaf up through the chain. This is the
  // property test_unwind cannot show (it uses fake stacks); here it is a real one.
  CHECK(incl_count["hp_top"] * 2 > static_cast<int>(samples.size()));

  // 5) THE LEAF-CALLER SKIP -- REPORTED, not asserted. hp_mid may or may not
  // appear depending on whether the compiler made hp_leaf a *frameless* leaf,
  // which is an optimization artifact of the TARGET build, not a property of the
  // sampler: at -O2 hp_leaf is frameless and its caller hp_mid is skipped (SPEC
  // 8.2 / CHALLENGES B8); at -O1 or under ASan, hp_leaf builds a frame and hp_mid
  // reappears. Asserting incl(hp_mid)==0 would wrongly pin a compiler artifact as
  // an invariant of our code -- which it is not (learned the hard way, see B8).
  if (incl_count["hp_mid"] == 0)
    std::printf(
        "  NOTE: hp_mid skipped -> hp_leaf is a FRAMELESS leaf at this opt level;"
        " its caller is lost (SPEC 8.2 / CHALLENGES B8)\n");
  else
    std::printf(
        "  NOTE: hp_mid present (%d) -> hp_leaf built a frame at this opt level;"
        " no leaf-skip here\n",
        incl_count["hp_mid"]);
}

RUN_ALL()
