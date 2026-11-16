// KVM Pico A — main firmware.
//
//   core 1: PIO-USB host (keyboard / mouse / dongle via the hub) -> HID parser
//           -> InputModel -> router -> active PC
//   core 0: USB device to the gaming PC (keyboard + mouse/media), console
//
// Board settings: see CLAUDE.md (Pico 2, Adafruit TinyUSB, 120 MHz, -O2, FS 64KB).
// Wiring: kvm_pins.h (pico_a). Tunables: kvm_config.h.
//
// Console (Pico A's COM port, 115200, newline): type "help".

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <kvm_config.h>
#include <kvm_pins.h>
#include <kvm_log.h>
#include <console.h>
#include <pc_output.h>
#include <usb_device.h>
#include "router.h"

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
}

static void cmdSrc(int, char **) { router::requestDescribe(); }

static void cmdStats(int, char **) {
  log::printf("uptime %lu s, host reports in %lu, reports sent to PC A %lu, log dropped %lu\n",
              (unsigned long)(millis() / 1000), (unsigned long)router::reportsIn(), (unsigned long)pcout::sentCount(),
              (unsigned long)log::dropped());
  log::printf("PC A: %s%s, LEDs %02x\n", usbdev::mounted() ? "mounted" : "not mounted",
              usbdev::suspended() ? " (suspended)" : "", usbdev::leds());
}

static const console::Command kCommands[] = {
  {"lat", "[reset] - latency histogram (input -> PC collected)", cmdLat},
  {"src", "- list attached HID interfaces", cmdSrc},
  {"stats", "- counters and PC state", cmdStats},
};

// ------------------------------------------------------------------ core 0
void setup() {
  usbdev::begin("KVM Pico A");
  pcout::begin();
  Serial.begin(115200);
  console::begin(kCommands, sizeof kCommands / sizeof kCommands[0],
                 "=== KVM Pico A (" __DATE__ " " __TIME__ ") ===");
  pinMode(ONBOARD_LED, OUTPUT);
}

void loop() {
  pcout::loop();
  console::loop();
  digitalWrite(ONBOARD_LED, usbdev::mounted());
}

// ------------------------------------------------------------------ core 1
void setup1() { router::begin(); }
void loop1() { router::task(); }
