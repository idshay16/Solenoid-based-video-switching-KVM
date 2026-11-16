// usb_host.h — Pico A's USB host side: PIO-USB + TinyUSB host + our HID parser.
// Everything here runs on core 1 (call begin() from setup1(), task() from loop1()).
//
// For each HID interface ("source", = TinyUSB HID instance index) it:
//   * forces report protocol and parses the report descriptor into a Layout
//     (fetching descriptors > 256 bytes itself; falling back to boot protocol
//     if the descriptor has nothing usable),
//   * decodes every report and hands a hid::Decoded to your hook, timestamped,
//   * forwards keyboard LED state (Caps/Num/Scroll) to every keyboard.
// It defines the tuh_* TinyUSB callbacks, so a sketch using it must not.
#pragma once
#include <stdint.h>
#include "hid_parser.h"

namespace kvm {
namespace host {

struct Hooks {
  // A decoded report. `rid` = report ID (0 if none). t_us = time_us_32() on arrival.
  void (*report)(uint8_t src, uint8_t rid, const hid::Decoded &d, uint32_t t_us) = nullptr;
  // Optional raw bytes (before decoding), e.g. for dumps.
  void (*raw)(uint8_t src, const uint8_t *r, uint16_t len, uint32_t t_us) = nullptr;
  void (*mounted)(uint8_t src) = nullptr;     // layout ready
  void (*unmounted)(uint8_t src) = nullptr;   // release its keys!
};

// Start the host on PIO-USB (pins from kvm_pins.h). Halts with a log message if
// sysclk is not a multiple of 12 MHz.
void begin(const Hooks &hooks);
void task();

// Keyboard LED bits (hid_reports.h led::*) to show on every attached keyboard.
// Cheap to call often: only changes are sent.
void setLeds(uint8_t leds);

static constexpr uint8_t kMaxSrc = 16;
bool active(uint8_t src);
const hid::Layout *layout(uint8_t src);
void vidPid(uint8_t src, uint16_t &vid, uint16_t &pid);

// Log a one-line summary of every active source (kvm::log).
void describe();

// Exposed for SOF phase lock (step 10): when true, begin() configures PIO-USB
// with skip_alarm_pool and the caller must run pio_usb_host_frame() every 1 ms.
extern bool g_external_frame_timer;

}  // namespace host
}  // namespace kvm
