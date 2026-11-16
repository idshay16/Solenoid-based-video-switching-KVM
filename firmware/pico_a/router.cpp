#include "router.h"
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <input_model.h>
#include <kvm_hot.h>
#include <kvm_log.h>
#include <pc_output.h>
#include <usb_device.h>
#include <usb_host.h>

using namespace kvm;

namespace router {

namespace {
InputModel g_model;                 // core 1 only
volatile uint32_t g_reports_in = 0;
volatile bool g_req_describe = false;

// Send the changes `f` (InputModel flags) to the active PC.
void KVM_HOT(deliver)(uint8_t f, const hid::Decoded &d, uint32_t t) {
  if (f & InputModel::KBD_CHANGED) pcout::pushKbd(g_model.kbd(), t);
  if (f & (InputModel::BTN_CHANGED | InputModel::MOTION)) pcout::pushMouse(g_model.buttons(), d.x, d.y, d.wheel, d.pan, t);
  if (f & InputModel::CONS_CHANGED) pcout::pushConsumer(g_model.consumer(), t);
}

void KVM_HOT(onReport)(uint8_t src, uint8_t rid, const hid::Decoded &d, uint32_t t) {
  g_reports_in = g_reports_in + 1;
  deliver(g_model.apply(src, rid, d), d, t);
}

void onUnmount(uint8_t src) {
  // A device unplugged while a key was down must not leave it stuck.
  hid::Decoded none;
  deliver(g_model.dropSource(src), none, time_us_32());
}
}  // namespace

void begin() {
  host::Hooks h;
  h.report = onReport;
  h.unmounted = onUnmount;
  host::begin(h);
}

void KVM_HOT(task)() {
  host::task();
  host::setLeds(usbdev::leds());   // lock LEDs follow the active PC
  if (g_req_describe) {
    g_req_describe = false;
    host::describe();
  }
}

void requestDescribe() { g_req_describe = true; }
uint32_t reportsIn() { return g_reports_in; }

}  // namespace router
