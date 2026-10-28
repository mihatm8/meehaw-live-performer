// Minimal host test harness shared by all module tests. Usage:
//   #include "check.h"
//   TEST(name) { CHECK(cond); CHECK_EQ(a, b); CHECK_NEAR(a, b, eps); }
//   int main() { return lp_test::run(); }
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <functional>
namespace lp_test {
struct Case { const char* name; std::function<void()> fn; };
inline std::vector<Case>& cases() { static std::vector<Case> c; return c; }
inline int& failures() { static int f = 0; return f; }
inline int run() {
  int failedCases = 0;
  for (auto& c : cases()) { int before = failures(); std::printf("[test] %s\n", c.name); c.fn(); if (failures() != before) { failedCases++; std::printf("  FAILED\n"); } }
  std::printf("%zu tests, %d failed cases, %d failed checks\n", cases().size(), failedCases, failures());
  return failedCases ? 1 : 0;
}
struct Reg { Reg(const char* n, std::function<void()> f) { cases().push_back({n, f}); } };
}
#define TEST(name) static void test_##name(); static lp_test::Reg reg_##name(#name, test_##name); static void test_##name()
#define CHECK(c) do { if (!(c)) { std::printf("  CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c); lp_test::failures()++; } } while (0)
#define CHECK_EQ(a, b) do { auto _a = (a); auto _b = (b); if (!(_a == _b)) { std::printf("  CHECK_EQ failed %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, (long long)_a, (long long)_b); lp_test::failures()++; } } while (0)
#define CHECK_NEAR(a, b, eps) do { double _a = (a), _b = (b); if (std::fabs(_a - _b) > (eps)) { std::printf("  CHECK_NEAR failed %s:%d: %s ~ %s (%g vs %g)\n", __FILE__, __LINE__, #a, #b, _a, _b); lp_test::failures()++; } } while (0)
