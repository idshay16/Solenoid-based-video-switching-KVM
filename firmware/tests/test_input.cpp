// InputModel (merge + switch suppression) and HidOut (coalescing queue) tests.
#include "test.h"
#include "input_model.h"
#include "hid_out.h"
#include <string.h>
#include <initializer_list>

using namespace kvm;
using hid::Decoded;

static Decoded kbd(uint8_t mods, std::initializer_list<uint8_t> keys) {
  Decoded d;
  d.what = Decoded::KBD;
  d.mods = mods;
  for (uint8_t k : keys) d.keys[k >> 5] |= 1u << (k & 31);
  return d;
}
static Decoded mouse(uint16_t buttons, int32_t x = 0, int32_t y = 0) {
  Decoded d;
  d.what = Decoded::MOUSE;
  d.buttons = buttons;
  d.x = x;
  d.y = y;
  return d;
}
static bool hasKey(const KbdReport &r, uint8_t u) {
  for (uint8_t k : r.keys) if (k == u) return true;
  return false;
}

// ------------------------------------------------------------------ InputModel
TEST(model_union_of_sources) {
  InputModel m;
  CHECK(m.apply(0, 0, kbd(0x02, {0x04})) & InputModel::KBD_CHANGED);
  m.apply(3, 1, kbd(0x20, {0x05}));
  KbdReport r = m.kbd();
  CHECK_EQ(r.modifiers, 0x22);
  CHECK(hasKey(r, 0x04));
  CHECK(hasKey(r, 0x05));
  // source 0 releases: source 3 still holds b + RShift
  m.apply(0, 0, kbd(0, {}));
  r = m.kbd();
  CHECK_EQ(r.modifiers, 0x20);
  CHECK(!hasKey(r, 0x04));
  CHECK(hasKey(r, 0x05));
}

TEST(model_newest_six_keys) {
  InputModel m;
  // press 8 keys one at a time: output must hold the 6 newest
  std::initializer_list<uint8_t> all = {4, 5, 6, 7, 8, 9, 10, 11};
  Decoded d;
  d.what = Decoded::KBD;
  for (uint8_t k : all) {
    d.keys[k >> 5] |= 1u << (k & 31);
    m.apply(0, 0, d);
  }
  KbdReport r = m.kbd();
  CHECK(!hasKey(r, 4));
  CHECK(!hasKey(r, 5));
  for (uint8_t k = 6; k <= 11; ++k) CHECK(hasKey(r, k));
  // release the newest: the 7th-newest comes back
  d.keys[0] &= ~(1u << 11);
  m.apply(0, 0, d);
  r = m.kbd();
  CHECK(hasKey(r, 5));
  CHECK(!hasKey(r, 11));
}

TEST(model_rollover_keeps_previous) {
  InputModel m;
  m.apply(0, 0, kbd(0, {0x04}));
  Decoded ro = kbd(0, {});
  ro.rollover = true;
  CHECK_EQ(m.apply(0, 0, ro) & InputModel::KBD_CHANGED, 0);
  CHECK(hasKey(m.kbd(), 0x04));
}

TEST(model_suppress_held_until_released) {
  InputModel m;
  m.apply(0, 0, kbd(0x02, {0x04}));     // holding Shift + A during the switch
  m.apply(1, 0, mouse(0x01));            // and dragging with left button
  m.suppressHeld();
  CHECK_EQ(m.kbd().modifiers, 0);
  CHECK(!hasKey(m.kbd(), 0x04));
  CHECK_EQ(m.buttons(), 0);
  CHECK(m.anySuppressed());
  // new key while still holding the suppressed ones: only the new key shows
  m.apply(0, 0, kbd(0x02, {0x04, 0x05}));
  CHECK_EQ(m.kbd().modifiers, 0);
  CHECK(hasKey(m.kbd(), 0x05));
  CHECK(!hasKey(m.kbd(), 0x04));
  // release Shift and A, press Shift again: Shift is live now
  m.apply(0, 0, kbd(0, {0x05}));
  m.apply(0, 0, kbd(0x02, {0x05}));
  CHECK_EQ(m.kbd().modifiers, 0x02);
  // mouse: release, press again -> visible
  m.apply(1, 0, mouse(0));
  CHECK(m.apply(1, 0, mouse(0x01)) & InputModel::BTN_CHANGED);
  CHECK_EQ(m.buttons(), 1);
  m.apply(0, 0, kbd(0, {}));
  m.apply(1, 0, mouse(0));
  CHECK(!m.anySuppressed());
}

TEST(model_drop_source_releases) {
  InputModel m;
  m.apply(2, 0, kbd(0x01, {0x29}));
  CHECK(m.dropSource(2) & InputModel::KBD_CHANGED);
  CHECK_EQ(m.kbd().modifiers, 0);
  CHECK(!m.anyHeld());
}

TEST(model_motion_flag_and_multiple_report_ids) {
  InputModel m;
  CHECK(m.apply(0, 2, mouse(0, 5, 0)) & InputModel::MOTION);
  // one source with two keyboard report IDs: snapshots are tracked separately
  m.apply(1, 1, kbd(0, {0x04}));
  m.apply(1, 4, kbd(0, {0x05}));
  CHECK(hasKey(m.kbd(), 0x04));
  CHECK(hasKey(m.kbd(), 0x05));
  m.apply(1, 4, kbd(0, {}));
  CHECK(hasKey(m.kbd(), 0x04));
}

// ------------------------------------------------------------------ HidOut
TEST(out_mouse_coalesces_motion) {
  HidOut o;
  o.pushMouse(0, 3, 1, 0, 0, 100);
  o.pushMouse(0, 4, -2, 1, 0, 200);
  o.pushMouse(0, 1, 0, 0, 0, 300);
  MouseReport r;
  uint32_t t;
  CHECK(o.popMouse(r, t));
  CHECK_EQ(r.x, 8);
  CHECK_EQ(r.y, -1);
  CHECK_EQ(r.wheel, 1);
  CHECK_EQ(t, 100);   // latency counts from the oldest input
  CHECK(!o.popMouse(r, t));
}

TEST(out_fast_click_not_lost) {
  HidOut o;
  // press and release inside one poll interval
  o.pushMouse(1, 0, 0, 0, 0, 1);
  o.pushMouse(0, 2, 0, 0, 0, 2);
  MouseReport r;
  uint32_t t;
  CHECK(o.popMouse(r, t));
  CHECK_EQ(r.buttons, 1);
  CHECK(o.popMouse(r, t));
  CHECK_EQ(r.buttons, 0);
  CHECK_EQ(r.x, 2);
  CHECK(!o.popMouse(r, t));
  // no change -> nothing queued
  o.pushMouse(0, 0, 0, 0, 0, 3);
  CHECK(!o.mousePending());
}

TEST(out_big_motion_split) {
  HidOut o;
  o.pushMouse(0, 40000, -70000, 300, 0, 1);
  MouseReport r;
  uint32_t t;
  int32_t sx = 0, sy = 0, sw = 0, n = 0;
  while (o.popMouse(r, t)) { sx += r.x; sy += r.y; sw += r.wheel; n++; }
  CHECK_EQ(sx, 40000);
  CHECK_EQ(sy, -70000);
  CHECK_EQ(sw, 300);
  CHECK_EQ(n, 3);
}

TEST(out_keyboard_edges_and_dedupe) {
  HidOut o;
  KbdReport a{}, b{};
  b.keys[0] = 0x04;
  o.pushKbd(b, 1);   // down
  o.pushKbd(a, 2);   // up within one poll
  o.pushKbd(a, 3);   // duplicate: ignored
  KbdReport r;
  uint32_t t;
  CHECK(o.popKbd(r, t));
  CHECK_EQ(r.keys[0], 0x04);
  CHECK(o.popKbd(r, t));
  CHECK_EQ(r.keys[0], 0);
  CHECK(!o.popKbd(r, t));
}

TEST(out_release_all) {
  HidOut o;
  KbdReport k{};
  k.modifiers = 0x02;
  o.pushKbd(k, 1);
  o.pushMouse(1, 0, 0, 0, 0, 1);
  o.pushConsumer(0xE9, 1);
  KbdReport r;
  MouseReport m;
  uint16_t c;
  uint32_t t;
  o.popKbd(r, t);
  o.popMouse(m, t);
  o.popConsumer(c, t);
  // PC now believes Shift, left button and Vol+ are held. More input arrives, then switch.
  KbdReport k2 = k;
  k2.keys[0] = 0x05;
  o.pushKbd(k2, 2);
  o.pushMouse(1, 50, 0, 0, 0, 2);
  o.releaseAll(3);
  CHECK(o.popKbd(r, t));
  CHECK_EQ(r.modifiers, 0);
  CHECK_EQ(r.keys[0], 0);
  CHECK(!o.popKbd(r, t));
  CHECK(o.popMouse(m, t));
  CHECK_EQ(m.buttons, 0);
  CHECK_EQ(m.x, 0);   // pending motion for the old PC is dropped
  CHECK(o.popConsumer(c, t));
  CHECK_EQ(c, 0);
  // releaseAll when nothing is held queues nothing
  o.releaseAll(4);
  CHECK(!o.anyPending());
}
