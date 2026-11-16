// input_model.h — merges every physical input source into ONE logical keyboard,
// mouse-button set and media key, and implements the switch-time rules.
// Pure C++ (host-testable).
//
// Sources are TinyUSB HID instance indices (keyboard, mouse cable and dongle each
// have one or more). A keyboard report replaces that source's key snapshot; the
// output is the union of all sources ("both keyboards hold Shift" still = Shift).
//
// Switching (smooth-transition rule 3): suppressHeld() marks everything currently
// held. Suppressed keys/buttons are hidden from the output until physically
// released, so the new PC never sees a key it didn't see go down (no ghost Shift,
// no drag that starts mid-way). Releasing on the OLD PC is HidOut::releaseAll().
#pragma once
#include <stdint.h>
#include "hid_parser.h"
#include "hid_reports.h"

namespace kvm {

struct KeySet {
  uint32_t w[8] = {};
  bool test(uint8_t u) const { return w[u >> 5] & (1u << (u & 31)); }
  void set(uint8_t u) { w[u >> 5] |= 1u << (u & 31); }
  void clear(uint8_t u) { w[u >> 5] &= ~(1u << (u & 31)); }
  bool any() const {
    for (auto x : w) if (x) return true;
    return false;
  }
  bool operator==(const KeySet &o) const {
    for (int i = 0; i < 8; ++i) if (w[i] != o.w[i]) return false;
    return true;
  }
};

class InputModel {
 public:
  static constexpr uint8_t kMaxSrc = 16;     // TinyUSB HID instance indices (CFG_TUH_HID = 12)
  static constexpr uint8_t kKbdSlots = 3;    // keyboard report IDs tracked per source

  enum : uint8_t { KBD_CHANGED = 1, BTN_CHANGED = 2, MOTION = 4, CONS_CHANGED = 8 };

  InputModel() { reset(); }
  void reset();

  // Apply one decoded report from `src`. `rid` = report ID (0 if none).
  // Returns which effective outputs changed (MOTION = report carried motion).
  uint8_t apply(uint8_t src, uint8_t rid, const hid::Decoded &d);

  // Source unplugged: its keys/buttons are released. Returns change flags.
  uint8_t dropSource(uint8_t src);

  // Switch time: hide everything currently held until it is released.
  void suppressHeld();

  // Effective state (what the active PC should see).
  KbdReport kbd() const { return kbd_; }
  uint8_t   buttons() const { return (uint8_t)(eff_btn_ & 0x1F); }   // 5 output buttons
  uint16_t  consumer() const { return eff_cons_; }

  // Raw physical state (for hotkeys / diagnostics).
  const KeySet &rawKeys() const { return raw_keys_; }
  uint8_t  rawMods() const { return raw_mods_; }
  uint16_t rawButtons() const { return raw_btn_; }
  bool     anyHeld() const { return raw_keys_.any() || raw_mods_ || raw_btn_ || raw_cons_; }
  bool     anySuppressed() const { return sup_keys_.any() || sup_mods_ || sup_btn_ || sup_cons_; }

 private:
  struct KbdSlot { bool used; uint8_t rid; uint8_t mods; KeySet keys; };
  struct Src { KbdSlot kbd[kKbdSlots]; uint16_t buttons; uint16_t cons; };

  uint8_t recompute();

  Src src_[kMaxSrc];
  KeySet raw_keys_, sup_keys_;
  uint8_t raw_mods_, sup_mods_;
  uint16_t raw_btn_, sup_btn_, eff_btn_;
  uint16_t raw_cons_, sup_cons_, eff_cons_;
  uint8_t order_[32];   // effective non-modifier keys, oldest first
  uint8_t n_order_;
  KbdReport kbd_;
};

}  // namespace kvm
