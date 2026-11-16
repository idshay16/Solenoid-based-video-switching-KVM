// Bring-up step 4 (on-device half): our HID parser + input model on the real
// keyboard / mouse / dongle. Same wiring as 03_host_dump. Nothing is sent to the
// PC as keyboard/mouse yet; this only prints what the parser understood.
//
// Host-side half: scripts\test.ps1 (parser unit tests in WSL g++).
//
// Serial Monitor (Pico A's COM port):
//   - on plug-in: one line per HID interface: what it is (kbd/mouse/consumer/leds)
//   - KBD  mods=.. keys=..     every keyboard change (merged across all devices)
//   - BTN  ..                  mouse button changes
//   - MOVE dx dy wheel pan     motion summed over 100 ms (only when moving)
//   - CONS ....                media key usage (0 = released)
// Commands:
//   l  LED test: cycles Num -> Caps -> Scroll -> off on the physical keyboard(s)
//   s  suppress-held test: everything you hold now disappears until released
//   d  list sources
//   h  help
//
// Pass: every key, all mouse buttons (incl. side buttons), wheel and media keys
// print correctly on BOTH the cable mouse and the dongle; 8+ held keys on an
// NKRO keyboard show the 6 newest; LED test lights the keyboard LEDs.

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <kvm_pins.h>
#include <kvm_log.h>
#include <usb_host.h>
#include <input_model.h>

using namespace kvm;

namespace {
InputModel g_model;   // core 1 only
int32_t g_mx = 0, g_my = 0, g_mw = 0, g_mp = 0;
uint32_t g_last_move_print = 0;
volatile bool g_req_suppress = false, g_req_describe = false;
volatile int8_t g_led_step = -1;

void printKbd() {
  KbdReport r = g_model.kbd();
  log::printf("KBD  mods=%02x keys=%02x %02x %02x %02x %02x %02x\n", r.modifiers, r.keys[0], r.keys[1], r.keys[2],
              r.keys[3], r.keys[4], r.keys[5]);
}

void onReport(uint8_t src, uint8_t rid, const hid::Decoded &d, uint32_t t) {
  (void)t;
  uint8_t f = g_model.apply(src, rid, d);
  if (f & InputModel::KBD_CHANGED) printKbd();
  if (f & InputModel::BTN_CHANGED) log::printf("BTN  %02x  (src %u)\n", g_model.buttons(), src);
  if (f & InputModel::CONS_CHANGED) log::printf("CONS %04x\n", g_model.consumer());
  g_mx += d.x; g_my += d.y; g_mw += d.wheel; g_mp += d.pan;
}

void onUnmount(uint8_t src) {
  uint8_t f = g_model.dropSource(src);
  if (f & InputModel::KBD_CHANGED) printKbd();
}
}  // namespace

void setup() {
  Serial.begin(115200);
}

void loop() {
  static bool greeted = false;
  if (!greeted && Serial) {
    greeted = true;
    Serial.println("\n=== 04_parser_live (KVM Pico A) ===  l=LED test  s=suppress held  d=sources  h=help");
  }
  if (Serial) log::drain(Serial, 256);
  if (Serial.available()) {
    switch (Serial.read()) {
      case 'l': g_led_step = 0; break;
      case 's': g_req_suppress = true; break;
      case 'd': g_req_describe = true; break;
      case 'h': greeted = false; break;
      default: break;
    }
  }
}

void setup1() {
  host::Hooks h;
  h.report = onReport;
  h.unmounted = onUnmount;
  host::begin(h);
}

void loop1() {
  host::task();
  const uint32_t now = millis();
  if (now - g_last_move_print >= 100) {
    g_last_move_print = now;
    if (g_mx || g_my || g_mw || g_mp) log::printf("MOVE %ld %ld %ld %ld\n", (long)g_mx, (long)g_my, (long)g_mw, (long)g_mp);
    g_mx = g_my = g_mw = g_mp = 0;
  }
  if (g_req_suppress) {
    g_req_suppress = false;
    g_model.suppressHeld();
    log::printf("suppressed everything held; release to clear\n");
    printKbd();
  }
  if (g_req_describe) { g_req_describe = false; host::describe(); }
  static uint32_t led_t = 0;
  if (g_led_step >= 0 && now - led_t >= 700) {
    led_t = now;
    static const uint8_t seq[] = {led::NUM, led::CAPS, led::SCROLL, 0};
    static const char *names[] = {"Num", "Caps", "Scroll", "off"};
    host::setLeds(seq[g_led_step]);
    log::printf("LED test: %s\n", names[g_led_step]);
    g_led_step = g_led_step == 3 ? -1 : g_led_step + 1;
  }
}
