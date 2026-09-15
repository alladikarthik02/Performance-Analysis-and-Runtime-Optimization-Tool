// Minimal dependency-free test harness.
// No gtest: this project's whole point is that it has no hidden dependencies,
// and a test framework you can read in one screen is easier to trust than one
// you install.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace testing {

struct Test {
  const char* name;
  void (*fn)();
};

inline std::vector<Test>& registry() {
  static std::vector<Test> r;
  return r;
}

inline int& failures() {
  static int f = 0;
  return f;
}

inline const char*& current() {
  static const char* c = "";
  return c;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void fail(const char* file, int line, const std::string& msg) {
  std::fprintf(stderr, "  FAIL %s:%d\n    %s\n", file, line, msg.c_str());
  failures()++;
}

inline int run_all() {
  int failed_tests = 0;
  for (auto& t : registry()) {
    current() = t.name;
    const int before = failures();
    t.fn();
    const bool ok = (failures() == before);
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", t.name);
    if (!ok) failed_tests++;
  }
  std::printf("\n%zu tests, %d failed\n", registry().size(), failed_tests);
  return failed_tests == 0 ? 0 : 1;
}

}  // namespace testing

#define TEST(name)                                            \
  static void name();                                         \
  static ::testing::Registrar reg_##name(#name, name);        \
  static void name()

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) ::testing::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
  } while (0)

#define CHECK_EQ(a, b)                                                     \
  do {                                                                     \
    auto _a = (a);                                                         \
    auto _b = (b);                                                         \
    if (!(_a == _b)) {                                                     \
      ::testing::fail(__FILE__, __LINE__,                                  \
                      "CHECK_EQ(" #a ", " #b ")  got: " +                  \
                          std::to_string(_a) + " vs " + std::to_string(_b)); \
    }                                                                      \
  } while (0)

#define RUN_ALL() int main() { return ::testing::run_all(); }
