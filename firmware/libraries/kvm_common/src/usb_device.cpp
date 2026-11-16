#include "usb_device.h"
#include <Adafruit_TinyUSB.h>

namespace kvm {
namespace usbdev {

namespace {

const uint8_t kKbdDesc[] = { TUD_HID_REPORT_DESC_KEYBOARD() };

const uint8_t kMouseDesc[] = {
  KVM_HID_DESC_MOUSE16,
  TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(RID_CONSUMER)),
};

// protocol, poll interval (ms), OUT endpoint
Adafruit_USBD_HID g_kbd(kKbdDesc, sizeof kKbdDesc, HID_ITF_PROTOCOL_KEYBOARD, 1, false);
Adafruit_USBD_HID g_mouse(kMouseDesc, sizeof kMouseDesc, HID_ITF_PROTOCOL_NONE, 1, false);

volatile uint8_t g_leds = 0;
volatile CompleteHook g_hook = nullptr;

void onKbdSetReport(uint8_t report_id, hid_report_type_t type, uint8_t const *buf, uint16_t len) {
  (void)report_id;
  if (type == HID_REPORT_TYPE_OUTPUT && len >= 1) g_leds = buf[0];
}

}  // namespace

void begin(const char *product) {
  if (!TinyUSBDevice.isInitialized()) TinyUSBDevice.begin(0);
  TinyUSBDevice.setProductDescriptor(product);

  g_kbd.setReportCallback(nullptr, onKbdSetReport);
  // Order matters: HID instance 0 = keyboard, 1 = mouse/consumer (ITF_KBD / ITF_MOUSE).
  g_kbd.begin();
  g_mouse.begin();

  // If the core already enumerated (e.g. CDC only), re-enumerate so the PC sees the HID interfaces.
  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }
}

void task() {
#ifdef TINYUSB_NEED_POLLING_TASK
  TinyUSBDevice.task();
#endif
}

bool mounted()      { return TinyUSBDevice.mounted(); }
bool suspended()    { return TinyUSBDevice.suspended(); }
bool remoteWakeup() { return TinyUSBDevice.suspended() && TinyUSBDevice.remoteWakeup(); }

bool kbdReady()   { return g_kbd.ready(); }
bool mouseReady() { return g_mouse.ready(); }

bool sendKbd(const KbdReport &r)     { return g_kbd.sendReport(0, &r, sizeof r); }
bool sendMouse(const MouseReport &r) { return g_mouse.sendReport(RID_MOUSE, &r, sizeof r); }
bool sendConsumer(uint16_t usage)    { return g_mouse.sendReport16(RID_CONSUMER, usage); }

uint8_t leds() { return g_leds; }

void setCompleteHook(CompleteHook hook) { g_hook = hook; }

}  // namespace usbdev
}  // namespace kvm

// TinyUSB weak callback: the PC has just collected a report from `instance`.
extern "C" void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len) {
  kvm::usbdev::CompleteHook hook = kvm::usbdev::g_hook;
  if (!hook) return;
  // For the keyboard (no report ID) report[0] is the modifier byte, not an ID.
  uint8_t rid = (instance == kvm::usbdev::ITF_MOUSE && len > 0) ? report[0] : 0;
  hook(instance, rid);
}
