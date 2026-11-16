#include "relays.h"
#include <hardware/gpio.h>
#include <kvm_config.h>
#include <kvm_pins.h>

using namespace kvm;

namespace relays {

namespace {
constexpr uint32_t kPinA = 1u << pico_a::RELAY_A, kPinB = 1u << pico_a::RELAY_B, kBoth = kPinA | kPinB;
volatile uint8_t g_state = 0;

uint32_t pins(uint8_t m) { return ((m & A) ? kPinA : 0) | ((m & B) ? kPinB : 0); }

// Runs during C++ static initialisation, i.e. before setup() and before the
// USB stack starts: the earliest point user code can touch the pins.
struct EarlyInit { EarlyInit() { safe(); } } g_early;
}  // namespace

void safe() {
  static constexpr int kPins[] = {pico_a::RELAY_A, pico_a::RELAY_B};
  for (int p : kPins) {
    gpio_init(p);            // SIO, input, output latch 0
    gpio_disable_pulls(p);   // the module's own resistor defines the idle level
  }
#if KVM_RELAY_OPEN_DRAIN
  // Off = high impedance (input); on = output driving LOW. Latch stays 0.
  gpio_put_masked(kBoth, 0);
  gpio_set_dir_masked(kBoth, 0);
#else
  gpio_put_masked(kBoth, KVM_RELAY_ACTIVE_LOW ? kBoth : 0);   // set the latch first...
  gpio_set_dir_masked(kBoth, kBoth);                           // ...then drive: no glitch
#endif
  g_state = 0;
}

void apply(uint8_t on, uint8_t off) {
  uint8_t s = (g_state & ~off) | on;
  const uint32_t on_pins = pins(s);
#if KVM_RELAY_OPEN_DRAIN
  gpio_set_dir_masked(kBoth, on_pins);   // energised pins become outputs (latch = 0 -> LOW)
#else
  gpio_put_masked(kBoth, KVM_RELAY_ACTIVE_LOW ? (kBoth & ~on_pins) : on_pins);
#endif
  g_state = s;
}

uint8_t state() { return g_state; }

}  // namespace relays
