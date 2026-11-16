// KVM Pico A — main firmware.
//
//   core 1: PIO-USB host (keyboard / mouse / dongle via the hub) -> HID parser
//           -> InputModel -> router -> active PC (A: pc_output, B: UART link)
//   core 0: USB device to the gaming PC, switch controller (lid button, relays,
//           press log), console
//
// Board settings: see CLAUDE.md (Pico 2, Adafruit TinyUSB, 120 MHz, -O2, FS 64KB).
// Wiring: kvm_pins.h (pico_a). Tunables: kvm_config.h.
//
// Console (Pico A's COM port, 115200, newline): type "help".

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <kvm_board.h>
#include <kvm_config.h>
#include <kvm_pins.h>
#include <kvm_log.h>
#include <console.h>
#include <pc_output.h>
#include <usb_device.h>
#include "link_a.h"
#include "relays.h"
#include "router.h"
#include "soak.h"
#include "switcher.h"

using namespace kvm;

// ------------------------------------------------------------------ console commands
static void cmdLat(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "reset")) {
    pcout::resetStats();
    log::printf("latency stats reset\n");
    return;
  }
  log::printf("added latency, input arrival -> gaming PC collected the report:\n");
  pcout::printStats();
  log::printf("(PC B: run 'lat' on Pico B's console)\n");
}

static void cmdSrc(int, char **) { router::requestDescribe(); }

static void cmdStats(int, char **) {
  log::printf("uptime %lu s, host reports in %lu, reports sent to PC A %lu, switches %lu, log dropped %lu\n",
              (unsigned long)(millis() / 1000), (unsigned long)router::reportsIn(), (unsigned long)pcout::sentCount(),
              (unsigned long)router::switchCount(), (unsigned long)log::dropped());
  log::printf("PC A: %s%s, LEDs %02x\n", usbdev::mounted() ? "mounted" : "not mounted",
              usbdev::suspended() ? " (suspended)" : "", usbdev::leds());
}

static void cmdLink(int, char **) { linka::printStatus(); }

static void cmdPing(int argc, char **argv) {
  linka::requestPing(argc > 1 ? (uint16_t)strtoul(argv[1], nullptr, 10) : 100);
}

static void cmdSwitch(int, char **) { switcher::press("console"); }

static void cmdState(int, char **) { switcher::printStatus(); }

static void cmdNudge(int argc, char **argv) {
  if (argc < 2 || (argv[1][0] != 'a' && argv[1][0] != 'b')) { log::printf("usage: nudge a|b\n"); return; }
  if (!switcher::nudge(argv[1][0] == 'a' ? relays::A : relays::B)) log::printf("busy (lockout), try again\n");
}

static void cmdBaseline(int argc, char **argv) {
  if (argc < 3) {
    log::printf("usage: baseline <A> <B>   what each monitor shows NOW: 1 = input 1 (gaming), 2 = input 2 (work)\n");
    return;
  }
  const uint8_t a = atoi(argv[1]) == 2, b = atoi(argv[2]) == 2;
  switcher::baseline((uint8_t)(a | (b << 1)));
}

static void cmdLog(int argc, char **argv) { switcher::printLog(argc > 1 ? strtoul(argv[1], nullptr, 10) : 20); }

static void cmdSoak(int argc, char **argv) {
  if (argc < 2) { soak::printReport(); return; }
  if (!strcmp(argv[1], "stop")) { soak::stop(); return; }
  soak::start(strtoul(argv[1], nullptr, 10) * 1000, argc > 2 ? strtoul(argv[2], nullptr, 10) : 0);
}

static const console::Command kCommands[] = {
  {"state", "- active PC, monitor inputs, press log", cmdState},
  {"switch", "- same as pressing the lid button", cmdSwitch},
  {"nudge", "a|b - press ONE monitor's DP switch (fix a monitor on the wrong input)", cmdNudge},
  {"baseline", "<A> <B> - tell me what the monitors show (1/2), no press", cmdBaseline},
  {"log", "[n] - last n press-log records", cmdLog},
  {"lat", "[reset] - latency histogram (input -> PC collected)", cmdLat},
  {"src", "- list attached HID interfaces", cmdSrc},
  {"stats", "- counters and PC A state", cmdStats},
  {"link", "- UART link status (B up?, errors, PC B state)", cmdLink},
  {"ping", "[n] - n round trips to B, RTT stats", cmdPing},
  {"soak", "[sec [count]] | stop - auto-switch every sec s; no args = report", cmdSoak},
};

// ------------------------------------------------------------------ core 0
void setup() {
  relays::safe();   // first: a relay must never click at boot (also done before main)
  usbdev::begin("KVM Pico A");
  pcout::begin();
  Serial.begin(115200);
  console::begin(kCommands, sizeof kCommands / sizeof kCommands[0],
                 "=== KVM Pico A (" __DATE__ " " __TIME__ ") ===");
  if (board::role() == board::B) log::printf("WARNING: this is Pico B's board (id %s) running the Pico A firmware\n", board::id());
  pinMode(ONBOARD_LED, OUTPUT);
  switcher::begin();   // press log + DP state (may erase flash: before the host starts)
  router::start();     // let core 1 start the USB host and the link
  soak::begin();       // watchdog on from here
}

void loop() {
  pcout::loop();
  switcher::loop();
  soak::loop();
  console::loop();
  digitalWrite(ONBOARD_LED, usbdev::mounted());
}

// ------------------------------------------------------------------ core 1
void setup1() { router::begin(); }
void loop1() { router::task(); }
