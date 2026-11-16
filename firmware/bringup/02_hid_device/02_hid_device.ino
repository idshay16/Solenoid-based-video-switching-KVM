// Bring-up step 2: Pico B as a USB keyboard + mouse + media keys. No wiring needed.
//
// Flash to Pico B and plug it into a second USB port on the gaming PC.
// Windows shows "KVM Pico B" as a keyboard, a mouse and a HID consumer device.
// Open the Serial Monitor (Pico B's COM port) and type a command + Enter:
//
//   m  mouse: draw a small square with the cursor (~1 s)
//   k  keyboard: after 3 s, types "kvm" (click into Notepad first)
//   v  media: volume up, then volume down (watch the Windows volume popup)
//   r  poll-rate test: 2 s of mouse reports sent back-to-back from the
//      "PC collected it" callback; prints reports/s and interval stats.
//      Expect ~1000/s and ~1000 us average (bInterval = 1 ms honoured).
//   l  print keyboard LED state
//   h  help
//
// LED: the on-board LED mirrors Caps Lock. Press Caps Lock on your real keyboard;
// Windows sends LED state to every keyboard, so the Pico's LED should follow and
// the change is printed. That proves the PC -> Pico LED path (needed for LED sync).

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <kvm_pins.h>
#include <usb_device.h>

using namespace kvm;

// ---------------------------------------------------------------- poll-rate test
namespace rate {
  volatile bool     active  = false;
  volatile uint32_t count   = 0;
  volatile uint32_t last_us = 0;
  volatile uint32_t min_us  = UINT32_MAX;
  volatile uint32_t max_us  = 0;
  volatile uint64_t sum_us  = 0;
  // histogram of intervals, 250 us buckets: [0,250) [250,500) ... [1750,2000) [2000+)
  volatile uint32_t hist[9];

  void reset() {
    count = 0; last_us = 0; min_us = UINT32_MAX; max_us = 0; sum_us = 0;
    for (auto &h : hist) h = 0;
  }
}

static const MouseReport kZeroMouse = {0, 0, 0, 0, 0};

// Runs in USB task context the moment the PC collects a report.
static void onComplete(uint8_t itf, uint8_t report_id) {
  if (!rate::active || itf != usbdev::ITF_MOUSE || report_id != RID_MOUSE) return;
  const uint32_t now = micros();
  if (rate::last_us) {
    const uint32_t d = now - rate::last_us;
    rate::count++;
    rate::sum_us += d;
    if (d < rate::min_us) rate::min_us = d;
    if (d > rate::max_us) rate::max_us = d;
    rate::hist[d >= 2000 ? 8 : d / 250]++;
  }
  rate::last_us = now;
  usbdev::sendMouse(kZeroMouse);   // chain the next report immediately: zero motion, cursor stays put
}

// ---------------------------------------------------------------- helpers
static bool waitReady(bool (*ready)(), uint32_t timeout_ms = 50) {
  const uint32_t t0 = millis();
  while (!ready()) {
    usbdev::task();
    if (millis() - t0 > timeout_ms) return false;
  }
  return true;
}

static void mouseMove(int16_t dx, int16_t dy) {
  MouseReport r = {0, dx, dy, 0, 0};
  if (waitReady(usbdev::mouseReady)) usbdev::sendMouse(r);
}

static void tapKey(uint8_t usage) {
  KbdReport down = {0, 0, {usage, 0, 0, 0, 0, 0}};
  KbdReport up   = {0, 0, {0, 0, 0, 0, 0, 0}};
  if (waitReady(usbdev::kbdReady)) usbdev::sendKbd(down);
  if (waitReady(usbdev::kbdReady)) usbdev::sendKbd(up);
}

static void tapConsumer(uint16_t usage) {
  if (waitReady(usbdev::mouseReady)) usbdev::sendConsumer(usage);
  if (waitReady(usbdev::mouseReady)) usbdev::sendConsumer(0);
}

static void printLeds(uint8_t l) {
  Serial.printf("LEDs: Num %s  Caps %s  Scroll %s\n",
                (l & led::NUM) ? "ON " : "off", (l & led::CAPS) ? "ON " : "off",
                (l & led::SCROLL) ? "ON " : "off");
}

static void help() {
  Serial.println("\n=== 02_hid_device (KVM Pico B) ===");
  Serial.println("  m  draw a square with the mouse");
  Serial.println("  k  type \"kvm\" after 3 s (focus Notepad)");
  Serial.println("  v  volume up then down");
  Serial.println("  r  poll-rate test (2 s)");
  Serial.println("  l  show keyboard LEDs");
  Serial.println("  h  this help");
  Serial.printf ("  USB mounted: %s\n", usbdev::mounted() ? "yes" : "no");
}

// ---------------------------------------------------------------- commands
static void cmdSquare() {
  Serial.println("mouse: square");
  const int8_t dirs[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
  for (auto &d : dirs)
    for (int i = 0; i < 50; ++i) mouseMove(d[0] * 2, d[1] * 2);   // 4 sides x 50 reports x 2 px
  Serial.println("mouse: done");
}

static void cmdType() {
  Serial.println("keyboard: typing \"kvm\" in 3 s...");
  delay(3000);
  tapKey(0x0E);  // k
  tapKey(0x19);  // v
  tapKey(0x10);  // m
  Serial.println("keyboard: done");
}

static void cmdVolume() {
  Serial.println("media: volume up, volume down");
  tapConsumer(0x00E9);  // Volume Increment
  delay(300);
  tapConsumer(0x00EA);  // Volume Decrement
}

static void cmdRate() {
  Serial.println("poll-rate test: 2 s...");
  noInterrupts(); rate::reset(); interrupts();
  if (!waitReady(usbdev::mouseReady)) { Serial.println("mouse endpoint not ready (mounted?)"); return; }
  rate::active = true;
  usbdev::sendMouse(kZeroMouse);   // kick off; onComplete keeps it going
  const uint32_t t0 = millis();
  while (millis() - t0 < 2000) usbdev::task();
  rate::active = false;

  const uint32_t n = rate::count;
  if (n == 0) { Serial.println("no reports collected; is the PC polling?"); return; }
  Serial.printf("  reports/s   %lu\n", (unsigned long)(n / 2));
  Serial.printf("  interval    avg %lu us   min %lu us   max %lu us\n",
                (unsigned long)(rate::sum_us / n), (unsigned long)rate::min_us,
                (unsigned long)rate::max_us);
  Serial.println("  histogram (us):");
  for (int i = 0; i < 9; ++i) {
    if (!rate::hist[i]) continue;
    if (i < 8) Serial.printf("    %4d-%4d  %lu\n", i * 250, i * 250 + 249, (unsigned long)rate::hist[i]);
    else       Serial.printf("    2000+      %lu\n", (unsigned long)rate::hist[i]);
  }
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  usbdev::begin("KVM Pico B");
  usbdev::setCompleteHook(onComplete);
  Serial.begin(115200);
  pinMode(ONBOARD_LED, OUTPUT);
}

void loop() {
  usbdev::task();

  static bool greeted = false;
  if (!greeted && Serial) { greeted = true; help(); }

  static uint8_t last_leds = 0xFF;
  const uint8_t l = usbdev::leds();
  if (l != last_leds) {
    last_leds = l;
    digitalWrite(ONBOARD_LED, (l & led::CAPS) ? HIGH : LOW);
    if (Serial) printLeds(l);
  }

  if (Serial.available()) {
    switch (Serial.read()) {
      case 'm': cmdSquare(); break;
      case 'k': cmdType();   break;
      case 'v': cmdVolume(); break;
      case 'r': cmdRate();   break;
      case 'l': printLeds(usbdev::leds()); break;
      case 'h': help();      break;
      default: break;   // ignore newlines etc.
    }
  }
}
