// Minimal test harness for host-side unit tests (no dependencies).
#pragma once
#include <stdio.h>
#include <stdint.h>

namespace t {
extern int failures, checks;
struct Reg { Reg(const char *name, void (*fn)()); };
}

#define TEST(name)                                         \
  static void name();                                      \
  static t::Reg reg_##name(#name, name);                   \
  static void name()

#define CHECK(c)                                                                  \
  do {                                                                            \
    t::checks++;                                                                  \
    if (!(c)) { t::failures++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
  } while (0)

#define CHECK_EQ(a, b)                                                            \
  do {                                                                            \
    t::checks++;                                                                  \
    long long va_ = (long long)(a), vb_ = (long long)(b);                         \
    if (va_ != vb_) {                                                             \
      t::failures++;                                                              \
      printf("  FAIL %s:%d: %s == %s  (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
    }                                                                             \
  } while (0)
