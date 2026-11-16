#include "switcher.h"
#include <Arduino.h>
#include <kvm_config.h>
#include <kvm_log.h>
#include <kvm_pins.h>
#include <presslog.h>
#include <switch_logic.h>
#include "flash_region.h"
#include "relays.h"
#include "router.h"

using namespace kvm;

namespace switcher {

namespace {
SwitchLogic g_logic({KVM_DP_PULSE_MS, KVM_DP_LOCKOUT_MS, KVM_INPUT_SWITCH_DELAY_MS, KVM_DP_FIRE_STAGGER_MS});
PressLog g_log(flashRegion());
bool g_log_ok = false;
uint8_t g_dp = 0;   // open-loop DP state: bit0 monitor A on input 2, bit1 monitor B on input 2
uint32_t g_presses = 0, g_collapsed = 0, g_log_full = 0;

// Button: idle HIGH (internal pull-up), pressed = LOW. Act once it has read LOW
// for KVM_BUTTON_CONFIRM_US (rejects noise spikes), re-arm after HIGH for
// KVM_BUTTON_DEBOUNCE_MS (ignores contact bounce on release).
bool g_armed = true;
uint32_t g_low_since = 0, g_high_since = 0;
bool g_low = false;

const char *dpName(uint8_t dp, uint8_t bit) { return (dp & bit) ? "input 2 (work)" : "input 1 (gaming)"; }

void logEvent(PressLog::Type type, uint8_t mask) {
  if (!g_log_ok) return;
  if (!g_log.append(type, g_dp, g_logic.target(), mask)) {
    g_log_full++;
    if (g_log_full == 1) log::printf("press log full; state kept in RAM, next boot compacts\n");
  }
}

void perform(const SwitchLogic::Actions &a) {
  // 1. Solenoids first (the slow mechanical part), both in one register write.
  if (a.relay_on || a.relay_off) relays::apply(a.relay_on, a.relay_off);
  // 2. Input moves now (KVM_INPUT_SWITCH_DELAY_MS = 0): core 1 performs it within microseconds.
  if (a.move_input) router::requestTarget(a.target ? router::PC_B : router::PC_A);
  // 3. Open-loop DP state + one flash record (~1 ms; both cores pause, at switch time only).
  if (a.pressed) {
    if (a.pressed & SwitchLogic::RELAY_A) g_dp ^= 1;
    if (a.pressed & SwitchLogic::RELAY_B) g_dp ^= 2;
    logEvent(a.move_input || a.pressed == 3 ? PressLog::PRESS : PressLog::NUDGE, a.pressed);
  }
}

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

void runNow() {
  const SwitchLogic::Actions a = g_logic.tick(millis());
  if (a.any()) perform(a);
}

void setTarget(uint8_t t) {
  g_logic.setTarget(t);
  router::requestTarget(t ? router::PC_B : router::PC_A);
}
}  // namespace

void begin() {
  relays::safe();
  gpio_init(pico_a::SWITCH_BTN);
  gpio_set_dir(pico_a::SWITCH_BTN, GPIO_IN);
  gpio_pull_up(pico_a::SWITCH_BTN);

  if (flashRegion().size() < 4096) {
    log::printf("press log DISABLED: no FS flash region (Tools > Flash Size must be 'FS: 64KB')\n");
  } else {
    g_log_ok = true;
    const bool had = g_log.mount();
    log::printf("press log: %lu/%lu records%s%s\n", (unsigned long)g_log.used(), (unsigned long)g_log.capacity(),
                g_log.compacted() ? ", compacted" : "", g_log.corrupt() ? ", some torn records skipped" : "");
#if KVM_DP_POWERON_MODE == KVM_DP_POWERON_REMEMBER
    if (had) {
      g_dp = g_log.latest().dp;
      log::printf("DP state restored from log (switches remember their input)\n");
    }
#else
    (void)had;
    g_dp = 0;   // switches come up on input 1 = gaming PC
#endif
  }
  // Input follows the video: both monitors on input 2 -> PC B, otherwise PC A.
  if (g_dp == 1 || g_dp == 2)
    log::printf("WARNING: monitors on different inputs (A %s, B %s) — fix with 'nudge a|b' or 'baseline'\n",
                dpName(g_dp, 1), dpName(g_dp, 2));
  setTarget(g_dp == 3 ? 1 : 0);
  logEvent(PressLog::BOOT, 0);
}

void loop() {
  pollButton();
  static uint32_t hot_seen = 0;
  const uint32_t hot = router::hotkeyCount();
  if (hot != hot_seen) {
    hot_seen = hot;
    press("hotkey");
  }
  runNow();
}

void press(const char *source) {
  g_presses++;
  if (g_logic.press(millis())) {
    log::printf("switch (%s) -> PC %c\n", source, g_logic.target() ? 'B' : 'A');
  } else {
    g_collapsed++;
    log::printf("switch (%s) during lockout: one toggle pending\n", source);
  }
  runNow();   // act in this loop iteration: no extra latency
}

bool nudge(uint8_t mask) {
  if (!g_logic.nudge(mask, millis())) return false;
  runNow();
  return true;
}

void baseline(uint8_t dp) {
  g_dp = dp & 3;
  if (g_dp == 0 || g_dp == 3) setTarget(g_dp == 3 ? 1 : 0);
  logEvent(PressLog::BASELINE, 0);
  printStatus();
}

void printStatus() {
  log::printf("input: PC %c%s | monitor A: %s | monitor B: %s | relays %s%s\n", g_logic.target() ? 'B' : 'A',
              g_logic.pending() ? " (toggle pending)" : "", dpName(g_dp, 1), dpName(g_dp, 2),
              relays::state() & relays::A ? "A" : "-", relays::state() & relays::B ? "B" : "-");
  log::printf("presses %lu (collapsed %lu), button %s, DP power-on mode %s\n", (unsigned long)g_presses,
              (unsigned long)g_collapsed, g_low ? "DOWN" : "up",
              KVM_DP_POWERON_MODE == KVM_DP_POWERON_REMEMBER ? "REMEMBER" : "PORT1");
  if (g_log_ok)
    log::printf("press log: %lu/%lu records, total presses ever %lu\n", (unsigned long)g_log.used(),
                (unsigned long)g_log.capacity(), (unsigned long)g_log.latest().presses);
}

void printLog(uint32_t n) {
  if (!g_log_ok) { log::printf("press log disabled\n"); return; }
  static const char *names[] = {"?", "BOOT", "PRESS", "NUDGE", "BASELINE"};
  const uint32_t used = g_log.used();
  for (uint32_t i = used > n ? used - n : 0; i < used; ++i) {
    PressRecord r;
    if (!g_log.record(i, r)) { log::printf("  #%lu (torn / invalid)\n", (unsigned long)i); continue; }
    log::printf("  seq %5lu %-8s dp A%c B%c target %c mask %u presses %lu\n", (unsigned long)r.seq,
                r.type <= 4 ? names[r.type] : "?", (r.dp & 1) ? '2' : '1', (r.dp & 2) ? '2' : '1',
                r.target ? 'B' : 'A', r.mask, (unsigned long)r.presses);
  }
}

}  // namespace switcher
