// relays.h — the two relay modules that drive the solenoids (kvm_pins.h:
// RELAY_A = GP10 header pin 14, RELAY_B = GP11 header pin 15).
// Polarity / drive mode: KVM_RELAY_ACTIVE_LOW, KVM_RELAY_OPEN_DRAIN (kvm_config.h),
// determined with bringup/08_relay_probe.
#pragma once
#include <stdint.h>

namespace relays {

constexpr uint8_t A = 1, B = 2;

// Drive both relays to "off". Runs automatically from a static constructor
// (before main/setup) and again from setup() — a relay must never click at boot.
void safe();

// Energise `on` and release `off` (bit masks) in ONE register write each, so
// both solenoids fire together.
void apply(uint8_t on, uint8_t off);
uint8_t state();

}  // namespace relays
