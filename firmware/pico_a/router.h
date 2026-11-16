// router.h — Pico A core 1: physical input -> InputModel -> active PC (A via
// pc_output, B via the UART link). Also performs the input side of a switch.
#pragma once
#include <stdint.h>

namespace router {

enum Target : uint8_t { PC_A = 0, PC_B = 1 };

void begin();   // setup1(): blocks until start() is called from core 0
void start();   // core 0, after boot-time flash work is done
void task();    // loop1()

// Core 0 -> core 1. The switch happens on core 1's next loop (microseconds):
// release everything on the old PC, suppress held keys, route to `t`.
void requestTarget(Target t);
Target target();          // currently active (as performed by core 1)
uint32_t switchCount();

void requestDescribe();
uint32_t reportsIn();
uint32_t hotkeyCount();   // increments on each hotkey press (KVM_HOTKEY_ENABLED)
uint32_t lastSwitchMs();  // millis() of the last performed switch
uint32_t loops();         // core 1 loop counter (liveness for the watchdog)

}  // namespace router
