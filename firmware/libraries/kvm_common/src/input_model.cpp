#include "input_model.h"
#include "kvm_hot.h"
#include <string.h>

namespace kvm {

void InputModel::reset() {
  for (auto &s : src_) s = Src{};
  raw_keys_ = sup_keys_ = KeySet();
  raw_mods_ = sup_mods_ = 0;
  raw_btn_ = sup_btn_ = eff_btn_ = 0;
  raw_cons_ = sup_cons_ = eff_cons_ = 0;
  n_order_ = 0;
  kbd_ = KbdReport{};
}

uint8_t KVM_HOT(InputModel::apply)(uint8_t src, uint8_t rid, const hid::Decoded &d) {
  if (src >= kMaxSrc) return 0;
  Src &s = src_[src];

  if ((d.what & hid::Decoded::KBD) && !d.rollover) {
    KbdSlot *slot = nullptr;
    for (auto &k : s.kbd)
      if (k.used && k.rid == rid) { slot = &k; break; }
    if (!slot)
      for (auto &k : s.kbd)
        if (!k.used) { slot = &k; break; }
    if (!slot) slot = &s.kbd[kKbdSlots - 1];   // more report IDs than slots: reuse the last
    slot->used = true;
    slot->rid = rid;
    slot->mods = d.mods;
    memcpy(slot->keys.w, d.keys, sizeof slot->keys.w);
  }
  if (d.what & hid::Decoded::MOUSE) s.buttons = d.buttons;
  if (d.what & hid::Decoded::CONS) s.cons = d.cons;

  uint8_t f = recompute();
  if (d.x || d.y || d.wheel || d.pan) f |= MOTION;
  return f;
}

uint8_t InputModel::dropSource(uint8_t src) {
  if (src >= kMaxSrc) return 0;
  src_[src] = Src{};
  return recompute();
}

void InputModel::suppressHeld() {
  sup_keys_ = raw_keys_;
  sup_mods_ = raw_mods_;
  sup_btn_ = raw_btn_;
  sup_cons_ = raw_cons_;
  recompute();
}

uint8_t KVM_HOT(InputModel::recompute)() {
  // 1. Raw union over all sources.
  KeySet keys;
  uint8_t mods = 0;
  uint16_t btn = 0, cons = 0;
  for (const Src &s : src_) {
    for (const KbdSlot &k : s.kbd) {
      if (!k.used) continue;
      mods |= k.mods;
      for (int i = 0; i < 8; ++i) keys.w[i] |= k.keys.w[i];
    }
    btn |= s.buttons;
    if (!cons) cons = s.cons;
  }
  raw_keys_ = keys;
  raw_mods_ = mods;
  raw_btn_ = btn;
  raw_cons_ = cons;

  // 2. Suppression ends per key once it is physically released.
  for (int i = 0; i < 8; ++i) sup_keys_.w[i] &= keys.w[i];
  sup_mods_ &= mods;
  sup_btn_ &= btn;
  if (sup_cons_ && cons != sup_cons_) sup_cons_ = 0;

  // 3. Effective = raw minus suppressed.
  KeySet eff;
  for (int i = 0; i < 8; ++i) eff.w[i] = keys.w[i] & ~sup_keys_.w[i];
  const uint8_t eff_mods = mods & ~sup_mods_;
  const uint16_t eff_btn = btn & ~sup_btn_;
  const uint16_t eff_cons = (sup_cons_ && cons == sup_cons_) ? 0 : cons;

  // 4. Press order: drop released keys, append newly pressed ones.
  uint8_t n = 0;
  for (uint8_t i = 0; i < n_order_; ++i)
    if (eff.test(order_[i])) order_[n++] = order_[i];
  n_order_ = n;
  for (int w = 0; w < 8; ++w) {
    uint32_t bits = eff.w[w];
    while (bits) {
      const int b = __builtin_ctz(bits);
      bits &= bits - 1;
      const uint8_t u = (uint8_t)(w * 32 + b);
      bool known = false;
      for (uint8_t i = 0; i < n_order_; ++i)
        if (order_[i] == u) { known = true; break; }
      if (!known) {
        if (n_order_ == sizeof order_) {   // 32 keys held: forget the oldest
          memmove(order_, order_ + 1, sizeof order_ - 1);
          n_order_--;
        }
        order_[n_order_++] = u;
      }
    }
  }

  // 5. Boot-keyboard output: modifiers + the 6 most recently pressed keys.
  KbdReport r{};
  r.modifiers = eff_mods;
  const uint8_t first = n_order_ > 6 ? n_order_ - 6 : 0;
  for (uint8_t i = first, j = 0; i < n_order_; ++i, ++j) r.keys[j] = order_[i];

  uint8_t f = 0;
  if (memcmp(&r, &kbd_, sizeof r) != 0) f |= KBD_CHANGED;
  if ((eff_btn & 0x1F) != (eff_btn_ & 0x1F)) f |= BTN_CHANGED;
  if (eff_cons != eff_cons_) f |= CONS_CHANGED;
  kbd_ = r;
  eff_btn_ = eff_btn;
  eff_cons_ = eff_cons;
  return f;
}

}  // namespace kvm
