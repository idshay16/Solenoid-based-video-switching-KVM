// soak.h — step 9: long-running robustness support on Pico A (core 0).
//
//  * Watchdog (1 s): fed only while BOTH cores make progress, so a hung USB host
//    on core 1 reboots the box instead of leaving the keyboard dead. Relays are
//    safe from the first instruction after reset; the DP state follows
//    KVM_DP_POWERON_MODE.
//  * Invariant monitor (always on): 200 ms after switching to PC B, PC A must
//    not believe anything is held (release-all worked). Violations are counted
//    and logged.
//  * "soak" console command: automatic switching every N seconds.
#pragma once
#include <stdint.h>

namespace soak {

void begin();   // end of setup()
void loop();    // every core-0 loop

void start(uint32_t period_ms, uint32_t count);
void stop();
void printReport();

}  // namespace soak
