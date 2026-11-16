#include "router.h"
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <input_model.h>
#include <kvm_config.h>
#include <kvm_hot.h>
#include <kvm_log.h>
#include <pc_output.h>
#include <usb_device.h>
#include <usb_host.h>
#include "link_a.h"

using namespace kvm;

namespace router {

namespace {
InputModel g_model;                       // core 1 only
volatile Target g_target = PC_A;          // written by core 1 only
volatile Target g_req_target = PC_A;      // written by core 0
volatile uint32_t g_reports_in = 0, g_switches = 0;
volatile bool g_req_describe = false;

// Send the changes `f` (InputModel flags) to the active PC.
void KVM_HOT(deliver)(uint8_t f, const hid::Decoded &d, uint32_t t) {
  if (g_target == PC_A) {
    if (f & InputModel::KBD_CHANGED) pcout::pushKbd(g_model.kbd(), t);
    if (f & (InputModel::BTN_CHANGED | InputModel::MOTION)) pcout::pushMouse(g_model.buttons(), d.x, d.y, d.wheel, d.pan, t);
    if (f & InputModel::CONS_CHANGED) pcout::pushConsumer(g_model.consumer(), t);
  } else {
    if (f & InputModel::KBD_CHANGED) linka::sendKbd(g_model.kbd());
    if (f & (InputModel::BTN_CHANGED | InputModel::MOTION)) linka::sendMouse(g_model.buttons(), d.x, d.y, d.wheel, d.pan);
    if (f & InputModel::CONS_CHANGED) linka::sendConsumer(g_model.consumer());
  }
}

volatile uint32_t g_hotkeys = 0;
bool g_hot_down = false;

void KVM_HOT(onReport)(uint8_t src, uint8_t rid, const hid::Decoded &d, uint32_t t) {
  g_reports_in = g_reports_in + 1;
  uint8_t f = g_model.apply(src, rid, d);
#if KVM_HOTKEY_ENABLED
  const bool hot = g_model.rawKeys().test(KVM_HOTKEY_KEY) && (g_model.rawMods() & KVM_HOTKEY_MODS) == KVM_HOTKEY_MODS;
  if (hot && !g_hot_down) {
    // Hide the hotkey (and its modifiers) from the current PC before anything
    // is delivered; core 0 turns the count into a switch press.
    g_model.suppressHeld();
    f |= InputModel::KBD_CHANGED | InputModel::BTN_CHANGED;
    g_hotkeys = g_hotkeys + 1;
  }
  g_hot_down = hot;
#endif
  deliver(f, d, t);
}

void onUnmount(uint8_t src) {
  // A device unplugged while a key was down must not leave it stuck.
  hid::Decoded none;
  deliver(g_model.dropSource(src), none, time_us_32());
}

void doSwitch(Target to) {
  const uint32_t t = time_us_32();
  // 1. Old PC: everything released, pending motion dropped.
  if (g_target == PC_A) pcout::releaseAll(t);
  else linka::sendReleaseAll();
  // 2. Keys / buttons still physically held stay invisible to the new PC until released.
  g_model.suppressHeld();
  // 3. Route.
  g_target = to;
  g_switches = g_switches + 1;
  log::printf("switch -> PC %c (%lu us)\n", to == PC_A ? 'A' : 'B', (unsigned long)(time_us_32() - t));
}
}  // namespace

void begin() {
  linka::begin();
  host::Hooks h;
  h.report = onReport;
  h.unmounted = onUnmount;
  host::begin(h);
}

void KVM_HOT(task)() {
  host::task();

  const Target req = g_req_target;
  if (req != g_target) doSwitch(req);

  // B rebooted / link came back: it starts with nothing held; don't let keys
  // that are already down appear on it out of nowhere via the heartbeat state.
  if (linka::takeHello() && g_target == PC_B) g_model.suppressHeld();

  linka::State st;
  st.b_active = g_target == PC_B;
  st.kbd = st.b_active ? g_model.kbd() : KbdReport{};
  st.buttons = st.b_active ? g_model.buttons() : 0;
  st.cons = st.b_active ? g_model.consumer() : 0;
  linka::task(st);

  // Lock LEDs on the physical keyboard follow the active PC.
  host::setLeds(g_target == PC_A ? usbdev::leds() : linka::ledsB());

  if (g_req_describe) {
    g_req_describe = false;
    host::describe();
  }
}

void requestTarget(Target t) { g_req_target = t; }
Target target() { return g_target; }
uint32_t switchCount() { return g_switches; }
void requestDescribe() { g_req_describe = true; }
uint32_t reportsIn() { return g_reports_in; }
uint32_t hotkeyCount() { return g_hotkeys; }

}  // namespace router
