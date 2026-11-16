#include "soak.h"
#include <Arduino.h>
#include <kvm_config.h>
#include <kvm_log.h>
#include <pc_output.h>
#include "link_a.h"
#include "router.h"
#include "switcher.h"

using namespace kvm;

namespace soak {

namespace {
constexpr uint32_t kWatchdogMs = 1000;

bool g_running = false;
uint32_t g_period = 0, g_left = 0, g_done = 0, g_next_ms = 0, g_start_ms = 0;
uint32_t g_violations = 0, g_checked_switch = 0;
uint32_t g_link_drops = 0;
bool g_link_was_up = false;

void checkInvariants() {
  const uint32_t sw = router::switchCount();
  if (sw != g_checked_switch && router::target() == router::PC_B && millis() - router::lastSwitchMs() > 200) {
    g_checked_switch = sw;
    if (pcout::pcHolds()) {
      g_violations++;
      log::printf("INVARIANT: PC A still holds a key/button 200 ms after switch #%lu\n", (unsigned long)sw);
    }
  }
  const bool up = linka::up();
  if (g_link_was_up && !up) g_link_drops++;
  g_link_was_up = up;
}

void feedWatchdog() {
  static uint32_t last_check = 0, last_loops = 0;
  if (millis() - last_check < 100) return;
  last_check = millis();
  const uint32_t loops = router::loops();
  if (loops != last_loops) {   // core 1 alive too
    last_loops = loops;
    rp2040.wdt_reset();
  }
}
}  // namespace

void begin() {
  if (rp2040.getResetReason() == RP2040::WDT_RESET) log::printf("NOTE: last reset was the WATCHDOG (a core hung)\n");
  rp2040.wdt_begin(kWatchdogMs);
}

void loop() {
  feedWatchdog();
  checkInvariants();
  if (g_running && (int32_t)(millis() - g_next_ms) >= 0) {
    g_next_ms += g_period;
    switcher::press("soak");
    g_done++;
    if (g_left && --g_left == 0) {
      g_running = false;
      log::printf("soak finished\n");
      printReport();
    }
  }
}

void start(uint32_t period_ms, uint32_t count) {
  if (period_ms < KVM_DP_LOCKOUT_MS + 100) period_ms = KVM_DP_LOCKOUT_MS + 100;
  g_period = period_ms;
  g_left = count;
  g_done = 0;
  g_violations = 0;
  g_link_drops = 0;
  g_start_ms = millis();
  g_next_ms = millis() + 200;
  g_running = true;
  pcout::resetStats();
  log::printf("soak: switching every %lu ms, %s; 'soak stop' to end, 'soak' for a report\n", (unsigned long)period_ms,
              count ? "limited" : "until stopped");
}

void stop() {
  if (g_running) {
    g_running = false;
    printReport();
  }
}

void printReport() {
  log::printf("=== soak report: %s, %lu switches in %lu s ===\n", g_running ? "RUNNING" : "stopped",
              (unsigned long)g_done, (unsigned long)((millis() - g_start_ms) / 1000));
  log::printf("  invariant violations (PC A still holding after switch): %lu\n", (unsigned long)g_violations);
  log::printf("  link drops: %lu, log bytes dropped: %lu\n", (unsigned long)g_link_drops, (unsigned long)log::dropped());
  linka::printStatus();
  switcher::printStatus();
  pcout::printStats();
}

}  // namespace soak
