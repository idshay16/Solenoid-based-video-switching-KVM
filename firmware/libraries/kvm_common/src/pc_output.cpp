#include "pc_output.h"
#include <Arduino.h>
#include <hardware/sync.h>
#include <string.h>
#include "hid_out.h"
#include "kvm_log.h"
#include "usb_device.h"

namespace kvm {
namespace pcout {

namespace {

HidOut g_out;
spin_lock_t *const g_lock = spin_lock_init(spin_lock_claim_unused(true));

// In-flight report per interface: arrival time of its oldest input (0 = none / not measured).
volatile uint32_t g_inflight_t[2] = {0, 0};
Stats g_stats[2];
volatile uint32_t g_sent = 0, g_wakeups = 0;
volatile uint32_t g_last_wakeup_ms = 0;
void (*volatile g_poll_observer)(uint8_t, uint32_t) = nullptr;

struct Lock {
  uint32_t save;
  Lock() : save(spin_lock_blocking(g_lock)) {}
  ~Lock() { spin_unlock(g_lock, save); }
};

void clearStats(Stats &s) {
  memset(&s, 0, sizeof s);
  s.min = UINT32_MAX;
}

void __not_in_flash_func(record)(uint8_t itf, uint32_t now) {
  const uint32_t t0 = g_inflight_t[itf];
  if (!t0) return;
  g_inflight_t[itf] = 0;
  const uint32_t d = now - t0;
  Stats &s = g_stats[itf];
  s.n++;
  s.sum += d;
  if (d < s.min) s.min = d;
  if (d > s.max) s.max = d;
  s.hist[d >= 2000 ? 20 : d / 100]++;
}

// Runs with the USB mutex held (IRQ hook or usbdev::service()).
void __not_in_flash_func(pump)() {
  if (!usbdev::mounted()) return;
  if (usbdev::suspended()) {
    // PC asleep: ask it to wake (at most every 100 ms); queue keeps the input.
    if (g_out.anyPending() && millis() - g_last_wakeup_ms > 100) {
      g_last_wakeup_ms = millis();
      if (usbdev::remoteWakeup()) g_wakeups = g_wakeups + 1;
    }
    return;
  }
  if (usbdev::kbdReady()) {
    KbdReport r;
    uint32_t t;
    bool ok;
    { Lock l; ok = g_out.popKbd(r, t); }
    if (ok && usbdev::sendKbd(r)) {
      g_inflight_t[usbdev::ITF_KBD] = t ? t : 1;
      g_sent = g_sent + 1;
    }
  }
  if (usbdev::mouseReady()) {
    // Consumer shares the mouse endpoint; media keys are rare, so they go first.
    uint16_t c;
    MouseReport m;
    uint32_t t;
    bool is_cons = false, ok;
    {
      Lock l;
      ok = g_out.popConsumer(c, t);
      if (ok) is_cons = true;
      else ok = g_out.popMouse(m, t);
    }
    if (ok && (is_cons ? usbdev::sendConsumer(c) : usbdev::sendMouse(m))) {
      g_inflight_t[usbdev::ITF_MOUSE] = t ? t : 1;
      g_sent = g_sent + 1;
    }
  }
}

void __not_in_flash_func(onComplete)(uint8_t itf, uint8_t report_id) {
  (void)report_id;
  const uint32_t now = time_us_32();
  if (itf < 2) record(itf, now);
  auto obs = g_poll_observer;
  if (obs) obs(itf, now);
  pump();
}

}  // namespace

void begin() {
  clearStats(g_stats[0]);
  clearStats(g_stats[1]);
  usbdev::setCompleteHook(onComplete);
}

void __not_in_flash_func(pushKbd)(const KbdReport &r, uint32_t t) { Lock l; g_out.pushKbd(r, t); }
void __not_in_flash_func(pushMouse)(uint8_t b, int32_t dx, int32_t dy, int32_t w, int32_t p, uint32_t t) {
  Lock l;
  g_out.pushMouse(b, dx, dy, w, p, t);
}
void __not_in_flash_func(pushConsumer)(uint16_t u, uint32_t t) { Lock l; g_out.pushConsumer(u, t); }
void releaseAll(uint32_t t) { Lock l; g_out.releaseAll(t); }
bool pending() { return g_out.anyPending(); }   // racy read is fine: it's a hint

void __not_in_flash_func(loop)() {
  // Only bother when something waits and its endpoint is free (otherwise the
  // completion callback will send it).
  if (!g_out.anyPending() || !usbdev::mounted()) return;
  const bool need = usbdev::suspended() || (g_out.kbdPending() && usbdev::kbdReady()) ||
                    ((g_out.mousePending() || g_out.consPending()) && usbdev::mouseReady());
  if (need) usbdev::service(pump);
}

void stats(Stats &kbd, Stats &mouse) {
  uint32_t save = save_and_disable_interrupts();
  kbd = g_stats[usbdev::ITF_KBD];
  mouse = g_stats[usbdev::ITF_MOUSE];
  restore_interrupts(save);
}

void resetStats() {
  uint32_t save = save_and_disable_interrupts();
  clearStats(g_stats[0]);
  clearStats(g_stats[1]);
  restore_interrupts(save);
}

void printStats() {
  Stats s[2];
  stats(s[0], s[1]);
  const char *names[2] = {"keyboard", "mouse/media"};
  for (int i = 0; i < 2; ++i) {
    if (!s[i].n) { log::printf("  %s: no reports yet\n", names[i]); continue; }
    log::printf("  %s: n=%lu  avg %lu us  min %lu  max %lu\n", names[i], (unsigned long)s[i].n,
                (unsigned long)(s[i].sum / s[i].n), (unsigned long)s[i].min, (unsigned long)s[i].max);
    for (int b = 0; b < 21; ++b) {
      if (!s[i].hist[b]) continue;
      const uint32_t pct = (uint32_t)((uint64_t)s[i].hist[b] * 100 / s[i].n);
      char bar[41];
      int len = (int)(pct * 40 / 100);
      memset(bar, '#', len);
      bar[len] = 0;
      if (b < 20) log::printf("    %4d-%4d %7lu %3lu%% %s\n", b * 100, b * 100 + 99, (unsigned long)s[i].hist[b], (unsigned long)pct, bar);
      else        log::printf("    2000+     %7lu %3lu%% %s\n", (unsigned long)s[i].hist[b], (unsigned long)pct, bar);
    }
  }
  log::printf("  sent %lu reports, remote wakeups %lu\n", (unsigned long)g_sent, (unsigned long)g_wakeups);
}

bool pcHolds() {
  Lock l;
  const KbdReport k = g_out.kbdSent();
  bool keys = k.modifiers != 0;
  for (uint8_t u : k.keys) keys = keys || u;
  return keys || g_out.buttonsSent() != 0;
}

uint32_t sentCount() { return g_sent; }
uint32_t wakeups() { return g_wakeups; }
void setPollObserver(void (*fn)(uint8_t, uint32_t)) { g_poll_observer = fn; }

}  // namespace pcout
}  // namespace kvm
