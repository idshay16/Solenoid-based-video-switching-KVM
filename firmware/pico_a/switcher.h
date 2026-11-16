// switcher.h — Pico A core 0: switch triggers (lid button, console, hotkey)
// -> SwitchLogic -> relays + router target + DP state + press log.
#pragma once
#include <stdint.h>

namespace switcher {

// setup(): mount the press log, restore DP state per KVM_DP_POWERON_MODE, pick
// the initial input target. May erase flash (only at boot, before the host runs).
void begin();
void loop();   // every core-0 loop

void press(const char *source);        // software trigger (console / hotkey)
bool nudge(uint8_t mask);              // press one DP switch only (1 = A, 2 = B)
void baseline(uint8_t dp);             // tell firmware what the monitors show (bit0 A, bit1 B: 1 = input 2)
void printStatus();
void printLog(uint32_t n);

}  // namespace switcher
