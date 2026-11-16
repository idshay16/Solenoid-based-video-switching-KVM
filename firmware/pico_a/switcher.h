// switcher.h — Pico A core 0: switch triggers (lid button, console, hotkey)
// -> SwitchLogic -> relays + router target + press log.
#pragma once
#include <stdint.h>

namespace switcher {

void begin();   // setup(), after the relay pins are already safe
void loop();    // every core-0 loop

void press(const char *source);   // software trigger (console / hotkey)
bool nudge(uint8_t mask);         // press one DP switch only (1 = A, 2 = B)
void printStatus();

}  // namespace switcher
