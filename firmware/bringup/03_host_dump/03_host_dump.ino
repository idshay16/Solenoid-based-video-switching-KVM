// Bring-up step 3: Pico A as a USB host (PIO-USB on GP16/GP17) through the hub.
// Dumps every device, its descriptors, and the raw HID reports.
//
// Wiring (see kvm_pins.h, pico_a): USB-A pigtail D+ (green) -> GP16 (pin 21),
// D- (white) -> GP17 (pin 22), GND -> any Pico A GND. Hub 5V from the 5V rail
// (or, for this test only, from Pico A VBUS pin 40 if the hub is light).
// Plug the hub's upstream into the pigtail; keyboard, mouse cable and dongle into
// the hub. Pico A's own USB-C goes to the gaming PC (serial console).
//
// Serial Monitor commands (Pico A's COM port):
//   r  toggle raw report printing (on at boot; mouse motion is ~1000 lines/s)
//   s  per-interface report statistics since the last 's' (rate, interval min/max)
//   d  re-dump descriptors of everything mounted
//   h  help
//
// What to send back: the full boot dump (everything up to the first "R " line),
// then a few report lines for: a key press, mouse move, each mouse button, wheel,
// and a media key if the keyboard has one. Those are the test vectors for step 4.

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <pio_usb.h>
#include <kvm_pins.h>
#include <kvm_log.h>

using namespace kvm;
#define LOG(...) kvm::log::printf(__VA_ARGS__)

Adafruit_USBH_Host USBHost;

// ------------------------------------------------------------------ per-device state (core 1)
namespace {

constexpr uint8_t kMaxDev = CFG_TUH_DEVICE_MAX + CFG_TUH_HUB + 1;   // daddr is 1-based

struct DevBuf {
  tusb_desc_device_t dev;
  uint8_t cfg[512];
  uint16_t str[64];
};
DevBuf g_dev[kMaxDev];

struct HidInfo {
  uint8_t daddr, itf_num, protocol;
  uint8_t rdesc[1024];
  uint32_t count;            // reports since last stats
  uint32_t last_us, min_us, max_us;
};
HidInfo g_hid[CFG_TUH_HID];

volatile bool g_print_reports = true;
volatile bool g_redump = false;
volatile bool g_stats = false;

const char *speedName(tusb_speed_t s) {
  return s == TUSB_SPEED_LOW ? "low" : s == TUSB_SPEED_FULL ? "full" : s == TUSB_SPEED_HIGH ? "high" : "?";
}

void hexdump(const char *prefix, const uint8_t *p, uint32_t n) {
  char line[96];
  for (uint32_t i = 0; i < n; i += 16) {
    int k = snprintf(line, sizeof line, "%s %04lx:", prefix, (unsigned long)i);
    for (uint32_t j = i; j < n && j < i + 16; ++j) k += snprintf(line + k, sizeof line - k, " %02x", p[j]);
    line[k++] = '\n';
    kvm::log::write(line, k);
  }
}

// ---- HID report descriptor item listing (human-readable, one item per line)
const char *itemName(uint8_t type, uint8_t tag) {
  static const char *mainN[16]   = {0, 0, 0, 0, 0, 0, 0, 0, "Input", "Output", "Collection", "Feature", "EndCollection", 0, 0, 0};
  static const char *globalN[16] = {"UsagePage", "LogMin", "LogMax", "PhysMin", "PhysMax", "UnitExp", "Unit", "ReportSize",
                                    "ReportID", "ReportCount", "Push", "Pop", 0, 0, 0, 0};
  static const char *localN[16]  = {"Usage", "UsageMin", "UsageMax", "DesigIdx", "DesigMin", "DesigMax", 0, "StringIdx",
                                    "StringMin", "StringMax", "Delim", 0, 0, 0, 0, 0};
  const char *n = type == 0 ? mainN[tag] : type == 1 ? globalN[tag] : type == 2 ? localN[tag] : nullptr;
  return n ? n : "?";
}

void listItems(uint8_t daddr, uint8_t inst, const uint8_t *d, uint32_t n) {
  uint32_t i = 0;
  int depth = 0;
  while (i < n) {
    uint8_t p = d[i];
    if (p == 0xFE) {   // long item
      if (i + 1 >= n) break;
      i += 3 + d[i + 1];
      continue;
    }
    uint8_t sz = p & 3;
    if (sz == 3) sz = 4;
    uint8_t type = (p >> 2) & 3, tag = p >> 4;
    uint32_t v = 0;
    for (uint8_t b = 0; b < sz && i + 1 + b < n; ++b) v |= (uint32_t)d[i + 1 + b] << (8 * b);
    if (type == 0 && tag == 0xC) depth--;
    LOG("RI d%u i%u %*s%s 0x%lx\n", daddr, inst, depth * 2, "", itemName(type, tag), (unsigned long)v);
    if (type == 0 && tag == 0xA) depth++;
    i += 1 + sz;
  }
}

void printHidDesc(uint8_t idx, uint32_t len) {
  HidInfo &h = g_hid[idx];
  LOG("HID d%u i%u itf%u protocol=%u (%s) report-desc %lu bytes\n", h.daddr, idx, h.itf_num, h.protocol,
      h.protocol == 1 ? "boot kbd" : h.protocol == 2 ? "boot mouse" : "none", (unsigned long)len);
  char pre[16];
  snprintf(pre, sizeof pre, "RD d%u i%u", h.daddr, idx);
  hexdump(pre, h.rdesc, len);
  listItems(h.daddr, idx, h.rdesc, len);
}

// ---- async descriptor chain per device: device -> config(9) -> config(full) -> strings
void onString(tuh_xfer_t *x);
void requestString(uint8_t daddr, uint8_t which);

void onConfigFull(tuh_xfer_t *x) {
  uint8_t a = x->daddr;
  if (x->result != XFER_RESULT_SUCCESS) { LOG("DEV d%u config read failed\n", a); return; }
  uint8_t *c = g_dev[a].cfg;
  uint16_t total = c[2] | (c[3] << 8);
  if (total > sizeof g_dev[a].cfg) total = sizeof g_dev[a].cfg;
  char pre[16];
  snprintf(pre, sizeof pre, "CFG d%u", a);
  hexdump(pre, c, total);
  // Walk interfaces / endpoints: bInterval is the device's native poll rate.
  for (uint16_t i = 0; i + 1 < total && c[i];) {
    uint8_t len = c[i], type = c[i + 1];
    if (type == TUSB_DESC_INTERFACE && len >= 9)
      LOG("  itf %u alt %u class %02x sub %02x proto %02x eps %u\n", c[i + 2], c[i + 3], c[i + 5], c[i + 6], c[i + 7], c[i + 4]);
    else if (type == TUSB_DESC_ENDPOINT && len >= 7)
      LOG("    ep 0x%02x %s maxpkt %u bInterval %u\n", c[i + 2],
          (c[i + 3] & 3) == 3 ? "interrupt" : (c[i + 3] & 3) == 2 ? "bulk" : "other", c[i + 4] | (c[i + 5] << 8), c[i + 6]);
    i += len;
  }
  requestString(a, 1);
}

void onConfigHeader(tuh_xfer_t *x) {
  uint8_t a = x->daddr;
  if (x->result != XFER_RESULT_SUCCESS) { LOG("DEV d%u config header failed\n", a); return; }
  uint16_t total = g_dev[a].cfg[2] | (g_dev[a].cfg[3] << 8);
  if (total > sizeof g_dev[a].cfg) total = sizeof g_dev[a].cfg;
  tuh_descriptor_get_configuration(a, 0, g_dev[a].cfg, total, onConfigFull, 0);
}

void onDevice(tuh_xfer_t *x) {
  uint8_t a = x->daddr;
  if (x->result != XFER_RESULT_SUCCESS) { LOG("DEV d%u device descriptor failed\n", a); return; }
  tusb_desc_device_t &d = g_dev[a].dev;
  tuh_bus_info_t bi = {};
  tuh_bus_info_get(a, &bi);
  LOG("DEV d%u vid %04x pid %04x bcdDevice %04x class %02x/%02x/%02x ep0 %u speed %s hub %u port %u\n", a,
      d.idVendor, d.idProduct, d.bcdDevice, d.bDeviceClass, d.bDeviceSubClass, d.bDeviceProtocol,
      d.bMaxPacketSize0, speedName(tuh_speed_get(a)), bi.hub_addr, bi.hub_port);
  if (d.bDeviceClass == TUSB_CLASS_HUB)
    LOG("  hub: %s\n", d.bDeviceProtocol == 2 ? "multi-TT" : d.bDeviceProtocol == 1 ? "single-TT" : "no TT (full-speed hub)");
  tuh_descriptor_get_configuration(a, 0, g_dev[a].cfg, 9, onConfigHeader, 0);
}

void requestString(uint8_t daddr, uint8_t which) {
  tusb_desc_device_t &d = g_dev[daddr].dev;
  uint8_t idx = which == 1 ? d.iManufacturer : which == 2 ? d.iProduct : which == 3 ? d.iSerialNumber : 0;
  while (which <= 3 && idx == 0) {
    ++which;
    idx = which == 2 ? d.iProduct : which == 3 ? d.iSerialNumber : 0;
  }
  if (which > 3) return;
  tuh_descriptor_get_string(daddr, idx, 0x0409, g_dev[daddr].str, sizeof g_dev[daddr].str, onString, which);
}

void onString(tuh_xfer_t *x) {
  uint8_t a = x->daddr, which = (uint8_t)x->user_data;
  if (x->result == XFER_RESULT_SUCCESS) {
    const uint16_t *s = g_dev[a].str;
    uint8_t n = ((s[0] & 0xFF) - 2) / 2;
    char out[64];
    uint8_t k = 0;
    for (uint8_t i = 0; i < n && k < sizeof out - 1; ++i) out[k++] = (s[1 + i] < 128) ? (char)s[1 + i] : '?';
    out[k] = 0;
    LOG("DEV d%u %s \"%s\"\n", a, which == 1 ? "manufacturer" : which == 2 ? "product" : "serial", out);
  }
  if (which < 3) requestString(a, which + 1);
}

void onHidDesc(tuh_xfer_t *x) {
  uint8_t idx = (uint8_t)x->user_data;
  if (x->result != XFER_RESULT_SUCCESS) { LOG("HID i%u report descriptor read failed\n", idx); return; }
  printHidDesc(idx, x->actual_len);
}

void dumpAll() {
  for (uint8_t a = 1; a < kMaxDev; ++a)
    if (tuh_mounted(a)) tuh_descriptor_get_device(a, &g_dev[a].dev, 18, onDevice, 0);
  for (uint8_t i = 0; i < CFG_TUH_HID; ++i)
    if (g_hid[i].daddr && tuh_hid_mounted(g_hid[i].daddr, i))
      tuh_descriptor_get_hid_report(g_hid[i].daddr, g_hid[i].itf_num, HID_DESC_TYPE_REPORT, 0, g_hid[i].rdesc,
                                    sizeof g_hid[i].rdesc, onHidDesc, i);
}

void printStats() {
  static uint32_t last_ms = 0;
  uint32_t now = millis(), dt = now - last_ms;
  last_ms = now;
  LOG("--- stats over %lu ms\n", (unsigned long)dt);
  for (uint8_t i = 0; i < CFG_TUH_HID; ++i) {
    HidInfo &h = g_hid[i];
    if (!h.daddr) continue;
    LOG("  d%u i%u: %lu reports (%lu/s) interval min %lu us max %lu us\n", h.daddr, i, (unsigned long)h.count,
        dt ? (unsigned long)(h.count * 1000ULL / dt) : 0, h.min_us == UINT32_MAX ? 0 : (unsigned long)h.min_us,
        (unsigned long)h.max_us);
    h.count = 0;
    h.min_us = UINT32_MAX;
    h.max_us = 0;
  }
}

}  // namespace

// ------------------------------------------------------------------ TinyUSB host callbacks (core 1)
extern "C" {

void tuh_mount_cb(uint8_t daddr) {
  LOG("MOUNT d%u\n", daddr);
  tuh_descriptor_get_device(daddr, &g_dev[daddr].dev, 18, onDevice, 0);
}

void tuh_umount_cb(uint8_t daddr) { LOG("UNMOUNT d%u\n", daddr); }

void tuh_hid_mount_cb(uint8_t daddr, uint8_t idx, uint8_t const *desc, uint16_t len) {
  HidInfo &h = g_hid[idx];
  tuh_itf_info_t info = {};
  tuh_hid_itf_get_info(daddr, idx, &info);
  h.daddr = daddr;
  h.itf_num = info.desc.bInterfaceNumber;
  h.protocol = tuh_hid_interface_protocol(daddr, idx);
  h.count = 0;
  h.last_us = 0;
  h.min_us = UINT32_MAX;
  h.max_us = 0;
  LOG("HID MOUNT d%u i%u itf%u (active protocol: %s)\n", daddr, idx, h.itf_num,
      tuh_hid_get_protocol(daddr, idx) == HID_PROTOCOL_REPORT ? "report" : "boot");
  if (desc && len) {
    memcpy(h.rdesc, desc, len < sizeof h.rdesc ? len : sizeof h.rdesc);
    printHidDesc(idx, len);
  } else {
    // Longer than TinyUSB's 256-byte enumeration buffer: fetch it ourselves.
    LOG("HID d%u i%u descriptor > 256 bytes, fetching\n", daddr, idx);
    tuh_descriptor_get_hid_report(daddr, h.itf_num, HID_DESC_TYPE_REPORT, 0, h.rdesc, sizeof h.rdesc, onHidDesc, idx);
  }
  if (!tuh_hid_receive_report(daddr, idx)) LOG("HID d%u i%u: receive_report failed\n", daddr, idx);
}

void tuh_hid_umount_cb(uint8_t daddr, uint8_t idx) {
  LOG("HID UNMOUNT d%u i%u\n", daddr, idx);
  g_hid[idx].daddr = 0;
}

void tuh_hid_report_received_cb(uint8_t daddr, uint8_t idx, uint8_t const *r, uint16_t len) {
  uint32_t now = time_us_32();
  HidInfo &h = g_hid[idx];
  uint32_t dt = h.last_us ? now - h.last_us : 0;
  if (h.last_us) {
    if (dt < h.min_us) h.min_us = dt;
    if (dt > h.max_us) h.max_us = dt;
  }
  h.last_us = now;
  h.count++;
  if (g_print_reports) {
    char line[200];
    int k = snprintf(line, sizeof line, "R d%u i%u +%lu:", daddr, idx, (unsigned long)dt);
    for (uint16_t i = 0; i < len && k < (int)sizeof line - 5; ++i) k += snprintf(line + k, sizeof line - k, " %02x", r[i]);
    line[k++] = '\n';
    kvm::log::write(line, k);
  }
  tuh_hid_receive_report(daddr, idx);
}

}  // extern "C"

// ------------------------------------------------------------------ core 0: console
void setup() {
  Serial.begin(115200);
  pinMode(ONBOARD_LED, OUTPUT);
}

void loop() {
  static bool greeted = false;
  if (!greeted && Serial) {
    greeted = true;
    Serial.println("\n=== 03_host_dump (KVM Pico A) ===  r=reports on/off  s=stats  d=redump  h=help");
    Serial.printf("sysclk %lu Hz, host D+ GP%u D- GP%u\n", (unsigned long)clock_get_hz(clk_sys), pico_a::USB_HOST_DP,
                  pico_a::USB_HOST_DM);
  }
  if (Serial) kvm::log::drain(Serial, 256);

  if (Serial.available()) {
    switch (Serial.read()) {
      case 'r': g_print_reports = !g_print_reports; Serial.printf("report printing %s\n", g_print_reports ? "ON" : "off"); break;
      case 's': g_stats = true; break;
      case 'd': g_redump = true; break;
      case 'h': greeted = false; break;
      default: break;
    }
  }

  static uint32_t blink = 0;
  if (millis() - blink > 500) { blink = millis(); digitalWrite(ONBOARD_LED, !digitalRead(ONBOARD_LED)); }
}

// ------------------------------------------------------------------ core 1: USB host
void setup1() {
  uint32_t hz = clock_get_hz(clk_sys);
  if (hz % 12000000UL) {
    LOG("ERROR: sysclk %lu Hz is not a multiple of 12 MHz; PIO-USB cannot run. Set CPU Speed 120 MHz.\n", (unsigned long)hz);
    while (true) tight_loop_contents();
  }
  pio_usb_configuration_t cfg = PIO_USB_DEFAULT_CONFIG;
  cfg.pin_dp = pico_a::USB_HOST_DP;   // D- = D+ + 1 (static_assert in kvm_pins.h)
  USBHost.configure_pio_usb(1, &cfg);
  // TinyUSB defaults HID devices to BOOT protocol (8-bit mouse, 3 buttons, 6KRO).
  // We want the full report format the device describes.
  tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);
  USBHost.begin(1);
  LOG("host started on PIO-USB\n");
}

void loop1() {
  USBHost.task();
  if (g_redump) { g_redump = false; dumpAll(); }
  if (g_stats) { g_stats = false; printStats(); }
}
