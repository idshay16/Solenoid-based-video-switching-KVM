#include "test.h"

namespace t {
int failures = 0, checks = 0;
struct Entry { const char *name; void (*fn)(); };
static Entry g_tests[256];
static int g_n = 0;
Reg::Reg(const char *name, void (*fn)()) { g_tests[g_n++] = {name, fn}; }
}  // namespace t

int main() {
  for (int i = 0; i < t::g_n; ++i) {
    int before = t::failures;
    t::g_tests[i].fn();
    printf("%s %s\n", t::failures == before ? "ok  " : "FAIL", t::g_tests[i].name);
  }
  printf("\n%d tests, %d checks, %d failures\n", t::g_n, t::checks, t::failures);
  return t::failures ? 1 : 0;
}
