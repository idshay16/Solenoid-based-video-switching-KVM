#include "switcher.h"
#include <Arduino.h>
#include <kvm_config.h>
#include <kvm_log.h>
#include <kvm_pins.h>
#include <switch_logic.h>
#include "router.h"

using namespace kvm;

namespace switcher {

namespace {
SwitchLogic g_logic({KVM_DP_PULSE_MS, KVM_DP_LOCKOUT_MS, KVM_INPUT_SWITCH_DELAY_MS, KVM_DP_FIRE_STAGGER_MS});
uint32_t g_presses = 0, g_collapsed = 0;

// Button: idle HIGH (internal pull-up), pressed = LOW. Act once it has read LOW
// for KVM_BUTTON_CONFIRM_US (rejects noise spikes), re-arm after HIGH for
// KVM_BUTTON_DEBOUNCE_MS (ignores contact bounce on release).
bool g_armed = true;
uint32_t g_low_since = 0, g_high_since = 0;
bool g_low = false;

void pollButton() {
  const bool low = !gpio_get(pico_a::SWITCH_BTN);
  const uint32_t now = time_us_32();
  if (low && !g_low) g_low_since = now;
  if (!low && g_low) g_high_since = now;
  g_low = low;
  if (g_armed && low && now - g_low_since >= KVM_BUTTON_CONFIRM_US) {
    g_armed = false;
    press("button");
  }
  if (!g_armed && !low && now - g_high_since >= KVM_BUTTON_DEBOUNCE_MS * 1000u) g_armed = true;
}

void perform(const SwitchLogic::Actions &a) {
  // Relays + press log are wired in by step 8.
  if (a.move_input) router::requestTarget(a.target ? router::PC_B : router::PC_A);
}
}  // namespace

void begin() {
  gpio_init(pico_a::SWITCH_BTN);
  gpio_set_dir(pico_a::SWITCH_BTN, GPIO_IN);
  gpio_pull_up(pico_a::SWITCH_BTN);
  g_logic.setTarget(router::PC_A);
}

void loop() {
  pollButton();
  static uint32_t hot_seen = 0;
  const uint32_t hot = router::hotkeyCount();
  if (hot != hot_seen) {
    hot_seen = hot;
    press("hotkey");
  }
  const SwitchLogic::Actions a = g_logic.tick(millis());
  if (a.any()) perform(a);
}

void press(const char *source) {
  g_presses++;
  if (g_logic.press(millis())) {
    log::printf("switch (%s) -> PC %c\n", source, g_logic.target() ? 'B' : 'A');
  } else {
    g_collapsed++;
    log::printf("switch (%s) during lockout: one toggle pending\n", source);
  }
  // Act in the same loop iteration: no extra latency.
  const SwitchLogic::Actions a = g_logic.tick(millis());
  if (a.any()) perform(a);
}

bool nudge(uint8_t mask) {
  if (!g_logic.nudge(mask, millis())) return false;
  const SwitchLogic::Actions a = g_logic.tick(millis());
  if (a.any()) perform(a);
  return true;
}

void printStatus() {
  log::printf("switch: target PC %c%s, presses %lu (collapsed %lu), button %s\n", g_logic.target() ? 'B' : 'A',
              g_logic.pending() ? ", toggle pending" : "", (unsigned long)g_presses, (unsigned long)g_collapsed,
              g_low ? "DOWN" : "up");
}

}  // namespace switcher
