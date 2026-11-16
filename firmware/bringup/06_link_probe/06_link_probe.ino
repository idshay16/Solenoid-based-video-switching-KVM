// Bring-up step 6: UART link A <-> B through the ADuM1201 — wiring check, baud
// sweep, error counts, round-trip time. Flash the SAME sketch to both Picos;
// each picks its role from its unique ID (kvm_board.h). Unknown board: set
// FORCE_ROLE below.
//
// Wiring (kvm_pins.h):
//   Pico A GP4 (pin 6)  -> ADuM pin 2 (VIA)      ADuM pin 7 (VOA) -> Pico B GP5 (pin 7)
//   Pico A GP5 (pin 7)  <- ADuM pin 3 (VOB)      ADuM pin 6 (VIB) <- Pico B GP4 (pin 6)
//   ADuM pin 1 VDD1 <- Pico A 3V3 (pin 36), pin 4 GND1 <- Pico A GND
//   ADuM pin 8 VDD2 <- Pico B 3V3 (pin 36), pin 5 GND2 <- Pico B GND
//   (Never connect the two GNDs. For this bench test both Picos may sit on the
//   gaming PC's USB; that's fine, it just doesn't test isolation.)
//
// Commands on Pico A's console:
//   probe          baud sweep 1..8 Mbaud: lost frames, CRC/UART errors, RTT
//   ping [n]       n round trips at the current baud (default 1000)
//   loop           SINGLE-BOARD test, no B / ADuM needed: jumper Pico A GP4 to
//                  GP5 and run this; it sweeps baud rates against itself
//   stats          counters
// Pico B's console just prints counters every 5 s (optional to open).
//
// Pass: "probe" shows 0 lost / 0 errors at 1 Mbaud and the recommended baud;
// put that into KVM_LINK_BAUD (kvm_config.h). Also read the ADuM marking:
// ADuM1201AR = 1 Mbps, BR = 10 Mbps, CR = 25 Mbps.

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <kvm_board.h>
#include <kvm_pins.h>
#include <kvm_log.h>
#include <console.h>
#include <link_uart.h>

// #define FORCE_ROLE 'A'

using namespace kvm;

// (helpers live in a namespace so the Arduino prototype generator leaves them alone)
namespace {

static constexpr uint32_t kBase = 1000000;
static const uint32_t kBauds[] = {1000000, 2000000, 3000000, 4000000, 6000000, 8000000};
static char g_role = 0;

// ------------------------------------------------------------------ helpers
static bool waitFrame(uint8_t want, uint32_t timeout_us, const uint8_t **pl = nullptr, size_t *plen = nullptr) {
  const uint32_t t0 = time_us_32();
  uint8_t type;
  const uint8_t *p;
  size_t n;
  while (time_us_32() - t0 < timeout_us) {
    linkuart::poll();
    while (linkuart::recv(type, p, n)) {
      if (type == want) {
        if (pl) *pl = p;
        if (plen) *plen = n;
        return true;
      }
    }
  }
  return false;
}

struct Result { uint32_t baud, actual, sent, ok, lost, bad_echo, crc, uart, b_err, rtt_min, rtt_avg, rtt_max; };

// n PINGs with a 32-byte payload (includes zeros and 0xFF), one at a time.
static Result pingBurst(uint32_t n, bool self_loop) {
  Result r = {};
  r.baud = r.actual = linkuart::baud();
  r.rtt_min = UINT32_MAX;
  uint64_t sum = 0;
  linkuart::resetStats();
  uint8_t payload[34];
  for (uint32_t i = 0; i < n; ++i) {
    payload[0] = (uint8_t)i;
    payload[1] = (uint8_t)(i >> 8);
    for (int k = 2; k < 34; ++k) payload[k] = (uint8_t)(i * 31 + k * 7);
    const uint32_t t0 = time_us_32();
    linkuart::send(link::PING, payload, sizeof payload);
    r.sent++;
    const uint8_t *p;
    size_t len;
    // self loop receives its own PING; a real B answers PONG
    if (waitFrame(self_loop ? link::PING : link::PONG, 5000, &p, &len)) {
      const uint32_t rtt = time_us_32() - t0;
      if (len == sizeof payload && !memcmp(p, payload, len)) {
        r.ok++;
        sum += rtt;
        if (rtt < r.rtt_min) r.rtt_min = rtt;
        if (rtt > r.rtt_max) r.rtt_max = rtt;
      } else {
        r.bad_echo++;
      }
    } else {
      r.lost++;
    }
    if ((i & 63) == 0) log::drain(Serial, 256);
  }
  r.crc = linkuart::rx().crcErrors() + linkuart::rx().framingErrors();
  r.uart = linkuart::uartErrors();
  r.rtt_avg = r.ok ? (uint32_t)(sum / r.ok) : 0;
  if (r.rtt_min == UINT32_MAX) r.rtt_min = 0;
  return r;
}

static void printResult(const Result &r) {
  log::printf("%5lu kbaud (actual %7lu): sent %4lu ok %4lu lost %4lu bad %3lu | rx err %3lu uart err %3lu B err %3lu | rtt %4lu/%4lu/%4lu us\n",
              (unsigned long)(r.baud / 1000), (unsigned long)r.actual, (unsigned long)r.sent, (unsigned long)r.ok,
              (unsigned long)r.lost, (unsigned long)r.bad_echo, (unsigned long)r.crc, (unsigned long)r.uart,
              (unsigned long)r.b_err, (unsigned long)r.rtt_min, (unsigned long)r.rtt_avg, (unsigned long)r.rtt_max);
}

static bool clean(const Result &r) { return r.ok == r.sent && !r.crc && !r.uart && !r.b_err && !r.bad_echo; }

static void recommend(const Result *res, int n) {
  int best = -1;
  for (int i = 0; i < n; ++i) {
    if (!clean(res[i])) break;
    best = i;
  }
  if (best < 0) { log::printf("\nNo clean baud rate. Check wiring / ADuM power (both sides!) / common grounds per side.\n"); return; }
  const int pick = best > 0 ? best - 1 : 0;   // one step of margin
  log::printf("\nhighest clean: %lu baud. Recommended: #define KVM_LINK_BAUD %luu  (kvm_config.h, both Picos)\n",
              (unsigned long)res[best].baud, (unsigned long)res[pick].baud);
}

// ------------------------------------------------------------------ role A
static void cmdPing(int argc, char **argv) {
  uint32_t n = argc > 1 ? strtoul(argv[1], nullptr, 10) : 1000;
  printResult(pingBurst(n, false));
}

static void cmdProbe(int, char **) {
  Result res[sizeof kBauds / sizeof kBauds[0]];
  int done = 0;
  for (uint32_t b : kBauds) {
    linkuart::setBaud(kBase);
    delay(20);
    // ask B to switch
    linkuart::send(link::SET_BAUD, &b, 4);
    if (!waitFrame(link::BAUD_OK, 100000)) {
      log::printf("%lu: B did not answer SET_BAUD at base rate — is B running 06_link_probe and wired?\n", (unsigned long)b);
      break;
    }
    linkuart::setBaud(b);
    delay(20);
    Result r = pingBurst(1000, false);
    r.baud = b;
    // B's view of errors at this baud
    link::Heartbeat hb = {};
    linkuart::send(link::HEARTBEAT, &hb, sizeof hb);
    const uint8_t *p;
    size_t len;
    if (waitFrame(link::ACK, 20000, &p, &len) && len >= sizeof(link::Ack)) r.b_err = ((const link::Ack *)p)->rx_errors;
    else if (!r.ok) r.b_err = 255;
    printResult(r);
    res[done++] = r;
    // go back to base; B reverts by itself after 500 ms without valid frames,
    // or immediately on SET_BAUD at the new rate
    linkuart::send(link::SET_BAUD, &kBase, 4);
    if (!waitFrame(link::BAUD_OK, 50000)) delay(600);   // B times out to base
    log::drain(Serial, 256);
  }
  linkuart::setBaud(kBase);
  recommend(res, done);
}

static void cmdLoop(int, char **) {
  log::printf("self loopback: GP%u (TX) must be jumpered to GP%u (RX), ADuM / B disconnected\n", pico_a::LINK_TX, pico_a::LINK_RX);
  Result res[sizeof kBauds / sizeof kBauds[0]];
  int done = 0;
  for (uint32_t b : kBauds) {
    linkuart::setBaud(b);
    delay(5);
    Result r = pingBurst(1000, true);
    r.baud = b;
    printResult(r);
    res[done++] = r;
  }
  linkuart::setBaud(kBase);
  recommend(res, done);
}

static void cmdStats(int, char **) {
  log::printf("role %c, baud %lu, frames %lu, crc %lu, framing %lu, uart err %lu, tx dropped %lu\n", g_role,
              (unsigned long)linkuart::baud(), (unsigned long)linkuart::rx().frames(),
              (unsigned long)linkuart::rx().crcErrors(), (unsigned long)linkuart::rx().framingErrors(),
              (unsigned long)linkuart::uartErrors(), (unsigned long)linkuart::txDropped());
}

}  // namespace

static const console::Command kCmdsA[] = {
  {"probe", "- baud sweep with B (1..8 Mbaud)", cmdProbe},
  {"ping", "[n] - n round trips at the current baud", cmdPing},
  {"loop", "- single-board test: jumper GP4->GP5 first", cmdLoop},
  {"stats", "- counters", cmdStats},
};
static const console::Command kCmdsB[] = {
  {"stats", "- counters", cmdStats},
};

namespace {

// ------------------------------------------------------------------ role B
static void serviceB() {
  static uint32_t last_valid = 0;
  uint8_t type;
  const uint8_t *p;
  size_t n;
  while (linkuart::recv(type, p, n)) {
    last_valid = millis();
    digitalWrite(ONBOARD_LED, !digitalRead(ONBOARD_LED));
    switch (type) {
      case link::PING: linkuart::send(link::PONG, p, n); break;
      case link::SET_BAUD:
        if (n >= 4) {
          uint32_t b;
          memcpy(&b, p, 4);
          linkuart::send(link::BAUD_OK, &b, 4);
          linkuart::setBaud(b);
          linkuart::resetStats();
        }
        break;
      case link::HEARTBEAT: {
        uint32_t e = linkuart::rx().crcErrors() + linkuart::rx().framingErrors() + linkuart::uartErrors();
        link::Ack a = {p[0], 0, 0, (uint8_t)(e > 255 ? 255 : e)};
        linkuart::send(link::ACK, &a, sizeof a);
        break;
      }
      default: break;
    }
  }
  if (linkuart::baud() != kBase && millis() - last_valid > 500) {
    linkuart::setBaud(kBase);   // lost A at a rate that doesn't work: fall back
    linkuart::resetStats();
  }
  static uint32_t last_print = 0;
  if (Serial && millis() - last_print > 5000) {
    last_print = millis();
    cmdStats(0, nullptr);
  }
}

}  // namespace

// ------------------------------------------------------------------ setup / loop
void setup() {
  Serial.begin(115200);
  pinMode(ONBOARD_LED, OUTPUT);
#ifdef FORCE_ROLE
  g_role = FORCE_ROLE;
#else
  g_role = (char)board::role();
#endif
  if (g_role == 'A') {
    linkuart::begin(pico_a::LINK_UART, pico_a::LINK_TX, pico_a::LINK_RX, kBase, false);
    console::begin(kCmdsA, sizeof kCmdsA / sizeof kCmdsA[0], "=== 06_link_probe: role A ===");
  } else if (g_role == 'B') {
    linkuart::begin(pico_b::LINK_UART, pico_b::LINK_TX, pico_b::LINK_RX, kBase, true);
    linkuart::send(link::HELLO, &link::kVersion, 1);
    console::begin(kCmdsB, sizeof kCmdsB / sizeof kCmdsB[0], "=== 06_link_probe: role B (answers A) ===");
  } else {
    console::begin(nullptr, 0, "=== 06_link_probe: UNKNOWN board — set FORCE_ROLE ===");
  }
  log::printf("board id %s\n", board::id());
}

void loop() {
  if (g_role == 'B') serviceB();
  else if (g_role == 'A') linkuart::poll();
  console::loop();
}
