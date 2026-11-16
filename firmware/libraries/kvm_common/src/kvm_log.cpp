#include "kvm_log.h"
#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <hardware/sync.h>

namespace kvm {
namespace log {

namespace {
constexpr size_t kSize = 16384;   // power of two
char g_buf[kSize];
volatile uint32_t g_head = 0;     // write index (monotonic)
volatile uint32_t g_tail = 0;     // read index (monotonic)
volatile uint32_t g_dropped = 0;

// One hardware spinlock, claimed during static init (core 0, before setup()).
// Interrupts are disabled on the calling core while held, so an IRQ on the same
// core can't deadlock against it.
spin_lock_t *const g_lock = spin_lock_init(spin_lock_claim_unused(true));
inline spin_lock_t *lock() { return g_lock; }
}  // namespace

void __not_in_flash_func(write)(const char *s, size_t n) {
  spin_lock_t *l = lock();
  uint32_t save = spin_lock_blocking(l);
  uint32_t free_ = kSize - (g_head - g_tail);
  if (n > free_) {
    g_dropped = g_dropped + 1;
  } else {
    for (size_t i = 0; i < n; ++i) g_buf[(g_head + i) & (kSize - 1)] = s[i];
    g_head = g_head + n;
  }
  spin_unlock(l, save);
}

void printf(const char *fmt, ...) {
  char tmp[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  if (n >= (int)sizeof tmp) n = sizeof tmp - 1;
  write(tmp, (size_t)n);
}

size_t drain(Print &out, size_t max) {
  uint32_t avail = g_head - g_tail;
  if (!avail) return 0;
  int room = out.availableForWrite();
  if (room <= 0) return 0;
  size_t n = avail;
  if (n > max) n = max;
  if (n > (size_t)room) n = (size_t)room;
  char tmp[256];
  if (n > sizeof tmp) n = sizeof tmp;
  uint32_t t = g_tail;
  for (size_t i = 0; i < n; ++i) tmp[i] = g_buf[(t + i) & (kSize - 1)];
  out.write((const uint8_t *)tmp, n);
  spin_lock_t *l = lock();
  uint32_t save = spin_lock_blocking(l);
  g_tail = t + n;
  spin_unlock(l, save);
  return n;
}

uint32_t dropped() { return g_dropped; }
size_t pending() { return g_head - g_tail; }

}  // namespace log
}  // namespace kvm
