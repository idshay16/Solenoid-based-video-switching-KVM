// link_a.h — Pico A side of the UART link. Core 1 only (core 1 owns the UART);
// core 0 reads the status getters and posts requests.
#pragma once
#include <stdint.h>
#include <hid_reports.h>

namespace linka {

void begin();

// What B should be showing right now (sent in every heartbeat; heals lost frames).
struct State {
  bool b_active;
  kvm::KbdReport kbd;
  uint8_t buttons;
  uint16_t cons;
};
// Run every core-1 loop: RX handling, heartbeat, ping requests.
void task(const State &now);

// Input for PC B (call only while B is the target).
void sendKbd(const kvm::KbdReport &r);
void sendMouse(uint8_t buttons, int32_t dx, int32_t dy, int32_t wheel, int32_t pan);
void sendConsumer(uint16_t usage);
void sendReleaseAll();

// Status (any core).
bool up();                 // B acked a heartbeat recently
uint8_t ledsB();           // PC B's lock LEDs
bool bMounted();
bool bSuspended();
bool takeHello();          // true once after B (re)booted or the link came back up

// Core 0 -> core 1: run n PING round trips and log RTT stats.
void requestPing(uint16_t n);
void printStatus();        // via kvm::log

// Raw frame hook for step 10 (POLL timing from B). Called on core 1.
void setPollHook(void (*fn)(const uint8_t *payload, uint8_t len));

}  // namespace linka
