#include "usb_host.h"
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <pio_usb.h>
#include "kvm_hot.h"
#include "kvm_log.h"
#include "kvm_pins.h"

namespace kvm {
namespace host {

bool g_external_frame_timer = false;

namespace {

Adafruit_USBH_Host g_host;
Hooks g_hooks;

struct Src {
  volatile bool active;      // layout ready, reports flow
  uint8_t daddr, itf_num, protocol;
  uint16_t vid, pid;
  hid::Layout layout;
  uint8_t led_buf[8];        // SET_REPORT payload; must outlive the async transfer
  uint8_t leds_sent;         // 0xFF = never
  bool led_busy;
};
Src g_src[kMaxSrc];
uint8_t g_rdesc[kMaxSrc][1024];   // report descriptor fetch buffers (> 256-byte descriptors)
volatile uint8_t g_leds = 0;

const char *kindsOf(const hid::Layout &l, char *buf) {
  buf[0] = 0;
  if (l.hasKeyboard()) strcat(buf, "kbd ");
  if (l.hasMouse()) strcat(buf, "mouse ");
  if (l.hasConsumer()) strcat(buf, "consumer ");
  if (l.has_leds) strcat(buf, "leds ");
  if (!buf[0]) strcat(buf, "(nothing used) ");
  return buf;
}

void finishMount(uint8_t idx, const uint8_t *desc, uint16_t len) {
  Src &s = g_src[idx];
  bool ok = hid::parse(desc, len, s.layout);
  const bool usable = s.layout.hasKeyboard() || s.layout.hasMouse() || s.layout.hasConsumer();
  if ((!ok || !usable) && (s.protocol == HID_ITF_PROTOCOL_KEYBOARD || s.protocol == HID_ITF_PROTOCOL_MOUSE)) {
    // Descriptor unusable but the interface supports boot protocol: use that.
    tuh_hid_set_protocol(s.daddr, idx, HID_PROTOCOL_BOOT);
    if (s.protocol == HID_ITF_PROTOCOL_KEYBOARD) hid::bootKeyboard(s.layout);
    else hid::bootMouse(s.layout);
    log::printf("host: d%u i%u descriptor unusable (%s), using boot protocol\n", s.daddr, idx, ok ? "no fields" : "malformed");
  }
  char kinds[48];
  log::printf("host: d%u i%u %04x:%04x itf%u %s(%u fields%s%s)\n", s.daddr, idx, s.vid, s.pid, s.itf_num,
              kindsOf(s.layout, kinds), s.layout.n, s.layout.has_report_ids ? ", report IDs" : "",
              s.layout.truncated ? ", TRUNCATED" : "");
  s.leds_sent = 0xFF;
  s.led_busy = false;
  s.active = true;
  if (g_hooks.mounted) g_hooks.mounted(idx);
  if (!tuh_hid_receive_report(s.daddr, idx)) log::printf("host: d%u i%u receive_report failed\n", s.daddr, idx);
}

void onReportDesc(tuh_xfer_t *x) {
  uint8_t idx = (uint8_t)x->user_data;
  if (x->result != XFER_RESULT_SUCCESS) {
    log::printf("host: i%u report descriptor fetch failed\n", idx);
    finishMount(idx, nullptr, 0);   // boot fallback if possible
    return;
  }
  finishMount(idx, g_rdesc[idx], (uint16_t)x->actual_len);
}

void onSetReportDone(uint8_t idx) {
  if (idx < kMaxSrc) g_src[idx].led_busy = false;
}

void pushLeds() {
  const uint8_t leds = g_leds;
  for (uint8_t i = 0; i < kMaxSrc; ++i) {
    Src &s = g_src[i];
    if (!s.active || !s.layout.has_leds || s.led_busy || s.leds_sent == leds) continue;
    const hid::Layout &l = s.layout;
    uint8_t n = 0;
    if (l.has_report_ids) s.led_buf[n++] = l.led_rid;
    uint8_t bytes = l.led_report_bytes > 7 ? 7 : (uint8_t)l.led_report_bytes;
    memset(s.led_buf + n, 0, bytes);
    for (uint8_t k = 0; k < l.led_count; ++k) {
      const uint8_t usage = l.led_first + k;   // 1 Num, 2 Caps, 3 Scroll
      if (usage >= 1 && usage <= 3 && (leds & (1u << (usage - 1)))) {
        const uint16_t bit = l.led_bit_off + k;
        if (bit / 8 < bytes) s.led_buf[n + bit / 8] |= 1u << (bit % 8);
      }
    }
    if (tuh_hid_set_report(s.daddr, i, l.led_rid, HID_REPORT_TYPE_OUTPUT, s.led_buf, n + bytes)) {
      s.led_busy = true;
      s.leds_sent = leds;
    }
  }
}

}  // namespace

void begin(const Hooks &hooks) {
  g_hooks = hooks;
  const uint32_t hz = clock_get_hz(clk_sys);
  if (hz % 12000000UL) {
    log::printf("host: ERROR sysclk %lu Hz is not a multiple of 12 MHz; set CPU Speed to 120 MHz\n", (unsigned long)hz);
    while (true) tight_loop_contents();
  }
  pio_usb_configuration_t cfg = PIO_USB_DEFAULT_CONFIG;
  cfg.pin_dp = pico_a::USB_HOST_DP;   // D- = D+ + 1 (static_assert in kvm_pins.h)
  cfg.skip_alarm_pool = g_external_frame_timer;
  g_host.configure_pio_usb(1, &cfg);
  tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);
  g_host.begin(1);
  log::printf("host: PIO-USB on GP%u/GP%u, sysclk %lu MHz%s\n", pico_a::USB_HOST_DP, pico_a::USB_HOST_DM,
              (unsigned long)(hz / 1000000), g_external_frame_timer ? ", external frame timer" : "");
}

void task() {
  g_host.task();
  pushLeds();
}

void setLeds(uint8_t leds) { g_leds = leds & 0x07; }

bool active(uint8_t src) { return src < kMaxSrc && g_src[src].active; }
const hid::Layout *layout(uint8_t src) { return active(src) ? &g_src[src].layout : nullptr; }
void vidPid(uint8_t src, uint16_t &vid, uint16_t &pid) {
  vid = src < kMaxSrc ? g_src[src].vid : 0;
  pid = src < kMaxSrc ? g_src[src].pid : 0;
}

void describe() {
  bool any = false;
  for (uint8_t i = 0; i < kMaxSrc; ++i) {
    const Src &s = g_src[i];
    if (!s.active) continue;
    any = true;
    char kinds[48];
    log::printf("  src %u: d%u %04x:%04x itf%u %s\n", i, s.daddr, s.vid, s.pid, s.itf_num, kindsOf(s.layout, kinds));
  }
  if (!any) log::printf("  (no HID devices)\n");
}

}  // namespace host
}  // namespace kvm

// ------------------------------------------------------------------ TinyUSB host callbacks (core 1)
using namespace kvm;
using namespace kvm::host;

extern "C" {

void tuh_mount_cb(uint8_t daddr) {
  uint16_t vid = 0, pid = 0;
  tuh_vid_pid_get(daddr, &vid, &pid);
  log::printf("host: device d%u %04x:%04x mounted\n", daddr, vid, pid);
}

void tuh_umount_cb(uint8_t daddr) { log::printf("host: device d%u removed\n", daddr); }

void tuh_hid_mount_cb(uint8_t daddr, uint8_t idx, uint8_t const *desc, uint16_t len) {
  if (idx >= kMaxSrc) return;
  Src &s = g_src[idx];
  s.active = false;
  tuh_itf_info_t info = {};
  tuh_hid_itf_get_info(daddr, idx, &info);
  s.daddr = daddr;
  s.itf_num = info.desc.bInterfaceNumber;
  s.protocol = tuh_hid_interface_protocol(daddr, idx);
  tuh_vid_pid_get(daddr, &s.vid, &s.pid);
  if (desc && len) {
    finishMount(idx, desc, len);
  } else if (!tuh_descriptor_get_hid_report(daddr, s.itf_num, HID_DESC_TYPE_REPORT, 0, g_rdesc[idx], sizeof g_rdesc[idx],
                                            onReportDesc, idx)) {
    finishMount(idx, nullptr, 0);
  }
}

void tuh_hid_umount_cb(uint8_t daddr, uint8_t idx) {
  (void)daddr;
  if (idx >= kMaxSrc) return;
  g_src[idx].active = false;
  log::printf("host: d%u i%u removed\n", daddr, idx);
  if (g_hooks.unmounted) g_hooks.unmounted(idx);
}

void KVM_HOT(tuh_hid_report_received_cb)(uint8_t daddr, uint8_t idx, uint8_t const *r, uint16_t len) {
  const uint32_t t = time_us_32();
  if (idx < kMaxSrc && g_src[idx].active) {
    if (g_hooks.raw) g_hooks.raw(idx, r, len, t);
    if (g_hooks.report) {
      hid::Decoded d;
      hid::decode(g_src[idx].layout, r, len, d);
      if (d.what) g_hooks.report(idx, g_src[idx].layout.has_report_ids && len ? r[0] : 0, d, t);
    }
  }
  tuh_hid_receive_report(daddr, idx);
}

void tuh_hid_set_report_complete_cb(uint8_t daddr, uint8_t idx, uint8_t report_id, uint8_t report_type, uint16_t len) {
  (void)daddr; (void)report_id; (void)report_type; (void)len;
  onSetReportDone(idx);
}

}  // extern "C"
