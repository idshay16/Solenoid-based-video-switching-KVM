// kvm_log.h — non-blocking log ring, callable from any core / IRQ.
//
// Hot code must never block on USB CDC (Serial.print can stall until the PC reads).
// Instead, log::printf() copies the text into a RAM ring under a tiny spinlock and
// returns. Core 0 calls log::drain(Serial) from loop() when there is time; text
// written before the Serial Monitor was opened stays buffered (up to the ring size).
// On overflow new text is dropped and counted (log::dropped()).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace arduino { class Print; }

namespace kvm {
namespace log {

void printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void write(const char *s, size_t n);

// Move up to `max` bytes to `out` without blocking (respects availableForWrite()).
// Returns bytes moved. Only call from one place (core 0 loop).
size_t drain(arduino::Print &out, size_t max = 256);

uint32_t dropped();
size_t pending();

}  // namespace log
}  // namespace kvm
