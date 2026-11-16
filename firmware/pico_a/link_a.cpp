#include "link_a.h"
#include <Arduino.h>
#include <string.h>
#include <kvm_config.h>
#include <kvm_hot.h>
#include <kvm_log.h>
#include <kvm_pins.h>
#include <link_proto.h>
#include <link_uart.h>

using namespace kvm;

namespace linka {

namespace {
volatile bool g_up = false;
volatile uint8_t g_leds_b = 0, g_flags_b = 0, g_b_errors = 0;
volatile bool g_hello = false;
uint32_t g_last_ack_ms = 0, g_last_hb_ms = 0;
uint8_t g_seq = 0;
uint32_t g_acks = 0, g_drops_up = 0;

// ping (non-blocking state machine on core 1)
volatile uint16_t g_ping_req = 0;
uint16_t g_ping_left = 0, g_ping_seq = 0, g_ping_ok = 0, g_ping_lost = 0;
uint32_t g_ping_t0 = 0, g_ping_min = 0, g_ping_max = 0;
uint64_t g_ping_sum = 0;
bool g_ping_wait = false;

void (*g_poll_hook)(const uint8_t *, uint8_t) = nullptr;

void sendPing() {
  uint8_t p[2] = {(uint8_t)g_ping_seq, (uint8_t)(g_ping_seq >> 8)};
  g_ping_t0 = time_us_32();
  linkuart::send(link::PING, p, sizeof p);
  g_ping_wait = true;
}

void pingDone() {
  log::printf("link ping: %u ok, %u lost, rtt min %lu avg %lu max %lu us (baud %lu)\n", g_ping_ok, g_ping_lost,
              (unsigned long)g_ping_min, (unsigned long)(g_ping_ok ? g_ping_sum / g_ping_ok : 0), (unsigned long)g_ping_max,
              (unsigned long)linkuart::baud());
}

void handle(uint8_t type, const uint8_t *p, size_t n) {
  switch (type) {
    case link::ACK:
      if (n >= sizeof(link::Ack)) {
        const link::Ack *a = (const link::Ack *)p;
        g_leds_b = a->leds;
        g_flags_b = a->flags;
        g_b_errors = a->rx_errors;
        g_last_ack_ms = millis();
        g_acks++;
        if (!g_up) {
          g_up = true;
          g_hello = true;   // link (re)established: router treats it like B rebooting
          log::printf("link: B up\n");
        }
      }
      break;
    case link::LEDS:
      if (n >= 1) g_leds_b = p[0];
      break;
    case link::HELLO:
      g_hello = true;
      log::printf("link: B booted (protocol v%u)\n", n ? p[0] : 0);
      break;
    case link::PONG:
      if (g_ping_wait && n >= 2 && (p[0] | (p[1] << 8)) == g_ping_seq) {
        const uint32_t rtt = time_us_32() - g_ping_t0;
        g_ping_wait = false;
        g_ping_ok++;
        g_ping_sum += rtt;
        if (rtt < g_ping_min) g_ping_min = rtt;
        if (rtt > g_ping_max) g_ping_max = rtt;
      }
      break;
    case link::POLL:
      if (g_poll_hook) g_poll_hook(p, (uint8_t)n);
      break;
    default: break;
  }
}
}  // namespace

void begin() {
  const uint32_t actual = linkuart::begin(pico_a::LINK_UART, pico_a::LINK_TX, pico_a::LINK_RX, KVM_LINK_BAUD, false);
  log::printf("link: UART%u GP%u/GP%u at %lu baud (asked %lu)\n", pico_a::LINK_UART, pico_a::LINK_TX, pico_a::LINK_RX,
              (unsigned long)actual, (unsigned long)KVM_LINK_BAUD);
}

void KVM_HOT(task)(const State &now) {
  linkuart::poll();
  uint8_t type;
  const uint8_t *p;
  size_t n;
  while (linkuart::recv(type, p, n)) handle(type, p, n);

  const uint32_t ms = millis();
  if (ms - g_last_hb_ms >= KVM_HEARTBEAT_MS) {
    g_last_hb_ms = ms;
    link::Heartbeat hb;
    hb.seq = g_seq++;
    hb.b_active = now.b_active;
    memcpy(hb.kbd, &now.kbd, 8);
    hb.buttons = now.buttons;
    hb.cons = now.cons;
    linkuart::send(link::HEARTBEAT, &hb, sizeof hb);
  }
  if (g_up && ms - g_last_ack_ms > KVM_HEARTBEAT_MS * KVM_HEARTBEAT_MISSES) {
    g_up = false;
    g_drops_up++;
    log::printf("link: B lost (no ack for %lu ms)\n", (unsigned long)(ms - g_last_ack_ms));
  }

  // ping requests from the console
  if (g_ping_req && !g_ping_left) {
    g_ping_left = g_ping_req;
    g_ping_req = 0;
    g_ping_ok = g_ping_lost = 0;
    g_ping_min = UINT32_MAX;
    g_ping_max = 0;
    g_ping_sum = 0;
    g_ping_seq++;
    sendPing();
  }
  if (g_ping_left) {
    const bool timeout = g_ping_wait && time_us_32() - g_ping_t0 > 20000;
    if (timeout) { g_ping_wait = false; g_ping_lost++; }
    if (!g_ping_wait) {
      if (--g_ping_left) {
        g_ping_seq++;
        sendPing();
      } else {
        if (g_ping_min == UINT32_MAX) g_ping_min = 0;
        pingDone();
      }
    }
  }
}

void KVM_HOT(sendKbd)(const KbdReport &r) { linkuart::send(link::KBD, &r, sizeof r); }

void KVM_HOT(sendMouse)(uint8_t buttons, int32_t dx, int32_t dy, int32_t wheel, int32_t pan) {
  // Split anything that doesn't fit the wire format (very rare: > 32767 counts in one host report).
  do {
    const int32_t x = dx > 32767 ? 32767 : dx < -32767 ? -32767 : dx;
    const int32_t y = dy > 32767 ? 32767 : dy < -32767 ? -32767 : dy;
    const int32_t w = wheel > 127 ? 127 : wheel < -127 ? -127 : wheel;
    const int32_t pn = pan > 127 ? 127 : pan < -127 ? -127 : pan;
    MouseReport m{buttons, (int16_t)x, (int16_t)y, (int8_t)w, (int8_t)pn};
    linkuart::send(link::MOUSE, &m, sizeof m);
    dx -= x; dy -= y; wheel -= w; pan -= pn;
  } while (dx || dy || wheel || pan);
}

void KVM_HOT(sendConsumer)(uint16_t usage) { linkuart::send(link::CONS, &usage, 2); }
void sendReleaseAll() { linkuart::send(link::RELEASE_ALL); }

bool up() { return g_up; }
uint8_t ledsB() { return g_leds_b; }
bool bMounted() { return g_flags_b & 1; }
bool bSuspended() { return g_flags_b & 2; }
bool takeHello() {
  if (!g_hello) return false;
  g_hello = false;
  return true;
}
void requestPing(uint16_t n) { g_ping_req = n ? n : 1; }
void setPollHook(void (*fn)(const uint8_t *, uint8_t)) { g_poll_hook = fn; }

void printStatus() {
  log::printf("link: %s, baud %lu, acks %lu, drops %lu | A rx: frames %lu crc %lu framing %lu uart %lu | tx dropped %lu | B rx errors %u\n",
              g_up ? "UP" : "down", (unsigned long)linkuart::baud(), (unsigned long)g_acks, (unsigned long)g_drops_up,
              (unsigned long)linkuart::rx().frames(), (unsigned long)linkuart::rx().crcErrors(),
              (unsigned long)linkuart::rx().framingErrors(), (unsigned long)linkuart::uartErrors(),
              (unsigned long)linkuart::txDropped(), g_b_errors);
  log::printf("PC B: %s%s, LEDs %02x\n", bMounted() ? "mounted" : "not mounted", bSuspended() ? " (suspended)" : "", g_leds_b);
}

}  // namespace linka
