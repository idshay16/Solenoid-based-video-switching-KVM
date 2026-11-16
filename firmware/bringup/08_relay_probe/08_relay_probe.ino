// Bring-up step 8: relay modules + solenoids + DP switches (Pico A).
// Finds the relay polarity / drive mode, tunes the pulse length, checks the 5V
// rail survives both solenoids firing together.
//
// Wiring (kvm_pins.h, pico_a):
//   relay A IN -> GP10 (pin 14), relay B IN -> GP11 (pin 15)
//   relay VCC -> 5V rail, relay GND -> rail GND (= Pico A GND)
//   relay COM -> 5V rail, relay NO -> solenoid +, solenoid - -> rail GND,
//   flyback diode across each solenoid (stripe/cathode to +)
// Pins start FLOATING (safe for any module type) until you choose a mode.
//
// Console (Pico A's COM port):
//   mode lo|od|hi   lo = active-low push-pull (typical opto module, default fw)
//                   od = active-low open-drain (if "lo" leaves the relay LED dim/on)
//                   hi = active-high push-pull (transistor modules with H trigger)
//   on a|b|ab / off   hold relays (watch the module LED, listen for the click)
//   pulse [ms]      both relays together, like a real switch (default 100 ms)
//   click a|b [ms]  one relay
//   burst [n]       n switch pulses 900 ms apart (rail sag / brownout test)
//   level           read the pins back
//
// Procedure + what to send back:
//   1. mode lo, then "on a" / "off": LED + click on "on", fully off on "off"?
//      -> KVM_RELAY_ACTIVE_LOW 1, KVM_RELAY_OPEN_DRAIN 0 (firmware default)
//      Relay already ON right after "mode lo", or LED glows dim on "off":
//      try "mode od" -> KVM_RELAY_OPEN_DRAIN 1. Clicks inverted: "mode hi" ->
//      KVM_RELAY_ACTIVE_LOW 0.
//   2. pulse 30 / 50 / 80 / 100 / 150: smallest that ALWAYS toggles both DP
//      switches; set KVM_DP_PULSE_MS to ~1.5x that.
//   3. burst 20: Pico A must not reboot (the boot line shows the reset reason).
//      If it does: KVM_DP_FIRE_STAGGER_MS 30.
//   4. DP power-on test (needs the switches on the 5V rail): put both monitors
//      on input 2 with "pulse", cut the rail, restore it. Repeat 5x. Monitors
//      back on input 1 every time -> KVM_DP_POWERON_MODE PORT1 (default);
//      still on input 2 -> REMEMBER.

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <hardware/gpio.h>
#include <kvm_config.h>
#include <kvm_pins.h>
#include <kvm_log.h>
#include <console.h>

using namespace kvm;

namespace {
enum Mode { FLOAT, LO, OD, HI };
Mode g_mode = FLOAT;
uint8_t g_on = 0;
const uint32_t kA = 1u << pico_a::RELAY_A, kB = 1u << pico_a::RELAY_B, kBoth = kA | kB;
const char *kModeNames[] = {"floating (not driven)", "active-low push-pull", "active-low open-drain", "active-high push-pull"};

uint32_t pinsOf(uint8_t m) { return ((m & 1) ? kA : 0) | ((m & 2) ? kB : 0); }

void drive(uint8_t on) {
  g_on = on;
  const uint32_t p = pinsOf(on);
  switch (g_mode) {
    case FLOAT: gpio_set_dir_masked(kBoth, 0); break;
    case LO: gpio_put_masked(kBoth, kBoth & ~p); gpio_set_dir_masked(kBoth, kBoth); break;
    case OD: gpio_put_masked(kBoth, 0); gpio_set_dir_masked(kBoth, p); break;
    case HI: gpio_put_masked(kBoth, p); gpio_set_dir_masked(kBoth, kBoth); break;
  }
}

uint8_t parseWhich(const char *s) {
  if (!s) return 0;
  return (strchr(s, 'a') ? 1 : 0) | (strchr(s, 'b') ? 2 : 0);
}

void cmdMode(int argc, char **argv) {
  if (argc < 2) { log::printf("mode: %s\n", kModeNames[g_mode]); return; }
  g_mode = !strcmp(argv[1], "lo") ? LO : !strcmp(argv[1], "od") ? OD : !strcmp(argv[1], "hi") ? HI : FLOAT;
  drive(0);
  log::printf("mode %s, relays OFF\n", kModeNames[g_mode]);
}

void cmdOn(int argc, char **argv) {
  if (g_mode == FLOAT) { log::printf("pick a mode first (mode lo)\n"); return; }
  uint8_t w = parseWhich(argc > 1 ? argv[1] : "ab");
  drive(g_on | w);
  log::printf("relay %s ON\n", argc > 1 ? argv[1] : "ab");
}

void cmdOff(int, char **) { drive(0); log::printf("relays OFF\n"); }

void pulse(uint8_t which, uint32_t ms) {
  const uint32_t t0 = micros();
  drive(which);   // one register write: both together
  delay(ms);
  drive(0);
  log::printf("pulse %s %lu ms (actual %lu us)\n", which == 3 ? "A+B" : which == 1 ? "A" : "B", (unsigned long)ms,
              (unsigned long)(micros() - t0));
}

void cmdPulse(int argc, char **argv) {
  if (g_mode == FLOAT) { log::printf("pick a mode first (mode lo)\n"); return; }
  pulse(3, argc > 1 ? strtoul(argv[1], nullptr, 10) : KVM_DP_PULSE_MS);
}

void cmdClick(int argc, char **argv) {
  if (g_mode == FLOAT) { log::printf("pick a mode first (mode lo)\n"); return; }
  uint8_t w = parseWhich(argc > 1 ? argv[1] : nullptr);
  if (!w) { log::printf("usage: click a|b [ms]\n"); return; }
  pulse(w, argc > 2 ? strtoul(argv[2], nullptr, 10) : KVM_DP_PULSE_MS);
}

void cmdBurst(int argc, char **argv) {
  if (g_mode == FLOAT) { log::printf("pick a mode first (mode lo)\n"); return; }
  const uint32_t n = argc > 1 ? strtoul(argv[1], nullptr, 10) : 20;
  for (uint32_t i = 0; i < n; ++i) {
    pulse(3, KVM_DP_PULSE_MS);
    console::loop();   // keep the log flowing
    delay(KVM_DP_LOCKOUT_MS + 100 - KVM_DP_PULSE_MS);
  }
  log::printf("burst done: %lu pulses, Pico A still running (no brownout)\n", (unsigned long)n);
}

void cmdLevel(int, char **) {
  log::printf("GP%u (A) reads %d, GP%u (B) reads %d, mode %s, on-mask %u\n", pico_a::RELAY_A, gpio_get(pico_a::RELAY_A),
              pico_a::RELAY_B, gpio_get(pico_a::RELAY_B), kModeNames[g_mode], g_on);
}

const console::Command kCmds[] = {
  {"mode", "lo|od|hi - drive mode (pins float until set)", cmdMode},
  {"on", "a|b|ab - energise and hold", cmdOn},
  {"off", "- release both", cmdOff},
  {"pulse", "[ms] - both relays together", cmdPulse},
  {"click", "a|b [ms] - one relay", cmdClick},
  {"burst", "[n] - n switch pulses (brownout test)", cmdBurst},
  {"level", "- read pins back", cmdLevel},
};

const char *resetName() {
  switch (rp2040.getResetReason()) {
    case RP2040::PWRON_RESET: return "power-on";
    case RP2040::RUN_PIN_RESET: return "RUN pin";
    case RP2040::SOFT_RESET: return "software";
    case RP2040::WDT_RESET: return "watchdog";
    case RP2040::DEBUG_RESET: return "debugger";
    case RP2040::GLITCH_RESET: return "GLITCH (supply)";
    case RP2040::BROWNOUT_RESET: return "BROWNOUT (rail sagged!)";
    default: return "unknown";
  }
}
}  // namespace

void setup() {
  for (uint32_t p : {(uint32_t)pico_a::RELAY_A, (uint32_t)pico_a::RELAY_B}) {
    gpio_init(p);   // input, floating: safe for every module type
    gpio_disable_pulls(p);
  }
  Serial.begin(115200);
  console::begin(kCmds, sizeof kCmds / sizeof kCmds[0], "=== 08_relay_probe (KVM Pico A) ===");
  log::printf("last reset: %s\n", resetName());
}

void loop() { console::loop(); }
