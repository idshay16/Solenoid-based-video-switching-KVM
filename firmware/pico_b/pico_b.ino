// KVM Pico B — main firmware. USB device to the work PC, fed by Pico A over the
// isolated UART link. Powered only by the work PC's USB (never the 5V rail).
//
//   UART RX (IRQ) -> COBS/CRC frames -> pc_output (coalescing queue) -> USB HID
//
// Safety rules:
//   * no valid frame for KVM_HEARTBEAT_MS * KVM_HEARTBEAT_MISSES -> release all
//     (Pico A lost power / cable pulled: nothing stays stuck on the work PC)
//   * every heartbeat carries the full held state; differences are corrected,
//     so a corrupted frame can't leave a key or button down
//   * when A says B is not the active PC, B shows nothing held
//
// Console (Pico B's COM port): help.

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <kvm_board.h>
#include <kvm_config.h>
#include <kvm_pins.h>
#include <kvm_log.h>
#include <console.h>
#include <link_proto.h>
#include <link_uart.h>
#include <pc_output.h>
#include <usb_device.h>

using namespace kvm;

namespace {
bool g_link_up = false;
bool g_active = false;
uint32_t g_last_rx_ms = 0;
uint32_t g_timeouts = 0;
uint32_t g_violations = 0;
uint8_t g_leds_sent = 0xFF;

// What we've pushed to the PC (for heartbeat healing).
KbdReport g_kbd{};
uint8_t g_buttons = 0;
uint16_t g_cons = 0;

void setKbd(const KbdReport &r, uint32_t t) { g_kbd = r; pcout::pushKbd(r, t); }
void setCons(uint16_t c, uint32_t t) { g_cons = c; pcout::pushConsumer(c, t); }

void releaseAll(uint32_t t) {
  pcout::releaseAll(t);
  g_kbd = KbdReport{};
  g_buttons = 0;
  g_cons = 0;
}

uint8_t rxErrors() {
  const uint32_t e = linkuart::rx().crcErrors() + linkuart::rx().framingErrors() + linkuart::uartErrors();
  return e > 255 ? 255 : (uint8_t)e;
}

void handle(uint8_t type, const uint8_t *p, size_t n, uint32_t t) {
  switch (type) {
    case link::KBD:
      if (g_active && n >= sizeof(KbdReport)) {
        KbdReport r;
        memcpy(&r, p, sizeof r);
        setKbd(r, t);
      }
      break;
    case link::MOUSE:
      if (g_active && n >= sizeof(MouseReport)) {
        MouseReport m;
        memcpy(&m, p, sizeof m);
        g_buttons = m.buttons;
        pcout::pushMouse(m.buttons, m.x, m.y, m.wheel, m.pan, t);
      }
      break;
    case link::CONS:
      if (g_active && n >= 2) setCons((uint16_t)(p[0] | (p[1] << 8)), t);
      break;
    case link::RELEASE_ALL:
      releaseAll(t);
      break;
    case link::HEARTBEAT:
      if (n >= sizeof(link::Heartbeat)) {
        link::Heartbeat hb;
        memcpy(&hb, p, sizeof hb);
        const bool was = g_active;
        g_active = hb.b_active;
        if (was && !g_active) releaseAll(t);
        if (g_active) {   // heal anything a lost frame left wrong
          if (memcmp(&g_kbd, hb.kbd, 8)) {
            KbdReport r;
            memcpy(&r, hb.kbd, 8);
            setKbd(r, t);
          }
          if (g_buttons != hb.buttons) {
            g_buttons = hb.buttons;
            pcout::pushMouse(hb.buttons, 0, 0, 0, 0, t);
          }
          if (g_cons != hb.cons) setCons(hb.cons, t);
        }
        link::Ack a;
        a.seq = hb.seq;
        a.leds = usbdev::leds();
        a.flags = (usbdev::mounted() ? 1 : 0) | (usbdev::suspended() ? 2 : 0);
        a.rx_errors = rxErrors();
        linkuart::send(link::ACK, &a, sizeof a);
        g_leds_sent = a.leds;
      }
      break;
    case link::PING:
      linkuart::send(link::PONG, p, n);
      break;
    default: break;
  }
}

void serviceLink() {
  const uint32_t t = time_us_32();
  uint8_t type;
  const uint8_t *p;
  size_t n;
  while (linkuart::recv(type, p, n)) {
    g_last_rx_ms = millis();
    if (!g_link_up) { g_link_up = true; log::printf("link: A up\n"); }
    handle(type, p, n, t);
  }
  linkuart::poll();

  if (g_link_up && millis() - g_last_rx_ms > KVM_HEARTBEAT_MS * KVM_HEARTBEAT_MISSES) {
    g_link_up = false;
    g_active = false;
    g_timeouts++;
    releaseAll(time_us_32());
    log::printf("link: A lost -> released everything on this PC\n");
  }

  // PC B changed its lock LEDs: tell A now (also sent in every ACK).
  const uint8_t leds = usbdev::leds();
  if (leds != g_leds_sent && g_link_up) {
    g_leds_sent = leds;
    linkuart::send(link::LEDS, &leds, 1);
  }
}

void cmdLat(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "reset")) { pcout::resetStats(); return; }
  log::printf("added latency on B, link frame arrival -> work PC collected:\n");
  pcout::printStats();
}

void cmdLink(int, char **) {
  log::printf("link: %s, %s, baud %lu, frames %lu, crc %lu, framing %lu, uart %lu, rx dropped %lu, timeouts %lu\n",
              g_link_up ? "UP" : "down", g_active ? "ACTIVE (input goes here)" : "inactive",
              (unsigned long)linkuart::baud(), (unsigned long)linkuart::rx().frames(),
              (unsigned long)linkuart::rx().crcErrors(), (unsigned long)linkuart::rx().framingErrors(),
              (unsigned long)linkuart::uartErrors(), (unsigned long)linkuart::rxDropped(), (unsigned long)g_timeouts);
  log::printf("PC B: %s%s, LEDs %02x | invariant violations %lu\n", usbdev::mounted() ? "mounted" : "not mounted",
              usbdev::suspended() ? " (suspended)" : "", usbdev::leds(), (unsigned long)g_violations);
}

const console::Command kCommands[] = {
  {"lat", "[reset] - latency histogram (link frame -> PC collected)", cmdLat},
  {"link", "- link status and counters", cmdLink},
};
}  // namespace

void setup() {
  usbdev::begin("KVM Pico B");
  pcout::begin();
  Serial.begin(115200);
  const uint32_t baud = linkuart::begin(pico_b::LINK_UART, pico_b::LINK_TX, pico_b::LINK_RX, KVM_LINK_BAUD, true);
  linkuart::send(link::HELLO, &link::kVersion, 1);
  console::begin(kCommands, sizeof kCommands / sizeof kCommands[0], "=== KVM Pico B (" __DATE__ " " __TIME__ ") ===");
  log::printf("link UART%u GP%u/GP%u at %lu baud\n", pico_b::LINK_UART, pico_b::LINK_TX, pico_b::LINK_RX, (unsigned long)baud);
  if (board::role() == board::A) log::printf("WARNING: this is Pico A's board (id %s) running the Pico B firmware\n", board::id());
  pinMode(ONBOARD_LED, OUTPUT);
  if (rp2040.getResetReason() == RP2040::WDT_RESET) log::printf("NOTE: last reset was the WATCHDOG\n");
  rp2040.wdt_begin(1000);
}

// Soak invariant: 200 ms after B stops being the target (switch away or link
// loss), the work PC must not believe anything is held.
static void checkInvariant() {
  static bool prev = false, armed = false;
  static uint32_t since = 0;
  if (prev && !g_active) { armed = true; since = millis(); }
  prev = g_active;
  if (armed && millis() - since > 200) {
    armed = false;
    if (pcout::pcHolds()) {
      g_violations++;
      log::printf("INVARIANT: PC B still holds a key/button 200 ms after becoming inactive\n");
    }
  }
}

void loop() {
  rp2040.wdt_reset();
  serviceLink();
  pcout::loop();
  checkInvariant();
  console::loop();
  // LED: solid = link up and active, slow blink = link up, off = no link
  digitalWrite(ONBOARD_LED, g_link_up && (g_active || (millis() / 500) % 2));
}
