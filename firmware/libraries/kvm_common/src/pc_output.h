// pc_output.h — everything between "we have new input for this PC" and "the PC
// collected the report". Used by both Picos (A: input from its own USB host;
// B: input from the UART link).
//
//  * Holds a HidOut (coalescing queue) behind a hardware spinlock, so the
//    producer may be on core 1 while reports are sent from core 0.
//  * Sends the next report from the "PC collected the previous one" callback
//    (zero wait when the PC polls back-to-back) and from loop() when the
//    endpoint is idle.
//  * Wakes a suspended PC (remote wakeup) when input arrives.
//  * Latency histogram: input arrival (t_us passed to push*) -> PC collected
//    the report carrying it. This is exactly the latency the KVM adds after the
//    host stack delivered the report.
#pragma once
#include <stdint.h>
#include "hid_reports.h"

namespace kvm {
namespace pcout {

// Call once in setup() after usbdev::begin().
void begin();

// Producer side, any core. t_us = time_us_32() when the input arrived.
void pushKbd(const KbdReport &r, uint32_t t_us);
void pushMouse(uint8_t buttons, int32_t dx, int32_t dy, int32_t wheel, int32_t pan, uint32_t t_us);
void pushConsumer(uint16_t usage, uint32_t t_us);
void releaseAll(uint32_t t_us);
bool pending();

// Core 0 loop(): sends queued reports when the endpoint is idle. Cheap when idle.
void loop();

// Latency statistics (microseconds).
struct Stats {
  uint32_t n, min, max;
  uint64_t sum;
  uint32_t hist[21];   // 100 us buckets 0..1999, [20] = 2000+
};
void stats(Stats &kbd, Stats &mouse);
void resetStats();
void printStats();     // via kvm::log

// True if the PC currently believes a key, modifier or mouse button is held
// (based on the last report it collected). Used by soak invariants.
bool pcHolds();

// Counters
uint32_t sentCount();
uint32_t wakeups();

// Optional observer of every "PC collected a report" event (core 0, IRQ context).
// Used by SOF phase lock to learn when this PC polls.
void setPollObserver(void (*fn)(uint8_t itf, uint32_t t_us));

}  // namespace pcout
}  // namespace kvm
