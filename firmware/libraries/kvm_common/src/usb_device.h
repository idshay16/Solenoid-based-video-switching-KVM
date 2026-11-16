// usb_device.h — the USB HID device both Picos present to their PC.
// See hid_reports.h for the interface/report layout.
//
// Usage (in setup(), before anything slow):
//   kvm::usbdev::begin("KVM Pico B");
// Every sketch must also #include <Adafruit_TinyUSB.h> (arduino-pico requirement).
#pragma once
#include <stdint.h>
#include "hid_reports.h"

namespace kvm {
namespace usbdev {

// Registers both HID interfaces and (re)enumerates. `product` shows in Windows Device Manager.
void begin(const char *product);

// Call from loop() on every iteration (no-op where the core runs the USB task itself).
void task();

bool mounted();
bool suspended();
bool remoteWakeup();   // ask a sleeping PC to wake; returns false if not allowed/not suspended

// Ready = the previous report on that endpoint has been collected by the PC.
// Sending while not ready fails (returns false); callers coalesce and retry.
bool kbdReady();
bool mouseReady();     // also gates consumer reports (shared endpoint)

bool sendKbd(const KbdReport &r);
bool sendMouse(const MouseReport &r);
bool sendConsumer(uint16_t usage);   // 0 = released

// Last keyboard LED state the PC sent (led::NUM / CAPS / SCROLL bits).
uint8_t leds();

// Called (from USB task context) right after the PC collects a report.
// `itf` is 0 for keyboard, 1 for mouse/consumer; `report_id` is 0 for keyboard.
// This is the "PC just polled us" moment: used to chain the next report
// immediately and, later, for latency stats and SOF phase lock.
using CompleteHook = void (*)(uint8_t itf, uint8_t report_id);
void setCompleteHook(CompleteHook hook);

constexpr uint8_t ITF_KBD   = 0;
constexpr uint8_t ITF_MOUSE = 1;

}  // namespace usbdev
}  // namespace kvm
