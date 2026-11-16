#include "hid_out.h"
#include "kvm_hot.h"
#include <string.h>

namespace kvm {

namespace {
inline bool sameKbd(const KbdReport &a, const KbdReport &b) { return memcmp(&a, &b, sizeof a) == 0; }
inline int32_t clampTo(int32_t v, int32_t lim) { return v > lim ? lim : v < -lim ? -lim : v; }
}  // namespace

void HidOut::reset() {
  kq_head_ = kq_n_ = 0;
  mq_head_ = mq_n_ = 0;
  cq_head_ = cq_n_ = 0;
  kbd_last_ = kbd_sent_ = KbdReport{};
  btn_last_ = btn_sent_ = 0;
  cons_last_ = cons_sent_ = 0;
}

// ------------------------------------------------------------------ keyboard
void KVM_HOT(HidOut::pushKbd)(const KbdReport &r, uint32_t t) {
  if (sameKbd(r, kbd_last_)) return;
  kbd_last_ = r;
  if (kq_n_ == kKbdQ) {   // full: the newest state wins (an intermediate state is lost)
    kq_[(kq_head_ + kq_n_ - 1) % kKbdQ].r = r;
    return;
  }
  kq_[(kq_head_ + kq_n_) % kKbdQ] = {r, t};
  kq_n_++;
}

bool KVM_HOT(HidOut::popKbd)(KbdReport &r, uint32_t &t) {
  if (!kq_n_) return false;
  r = kq_[kq_head_].r;
  t = kq_[kq_head_].t;
  kq_head_ = (kq_head_ + 1) % kKbdQ;
  kq_n_--;
  kbd_sent_ = r;
  return true;
}

// ------------------------------------------------------------------ mouse
void KVM_HOT(HidOut::pushMouse)(uint8_t buttons, int32_t dx, int32_t dy, int32_t wheel, int32_t pan, uint32_t t) {
  const bool motion = dx || dy || wheel || pan;
  if (mq_n_) {
    MEnt &tail = mq_[(mq_head_ + mq_n_ - 1) % kMouseQ];
    if (tail.buttons == buttons || mq_n_ == kMouseQ) {
      // Same buttons (or queue full): fold the motion into the waiting report.
      tail.buttons = buttons;
      tail.x += dx;
      tail.y += dy;
      tail.wheel += wheel;
      tail.pan += pan;
      btn_last_ = buttons;
      return;
    }
  } else if (!motion && buttons == btn_last_) {
    return;   // nothing new
  }
  mq_[(mq_head_ + mq_n_) % kMouseQ] = {buttons, dx, dy, wheel, pan, t};
  mq_n_++;
  btn_last_ = buttons;
}

bool KVM_HOT(HidOut::popMouse)(MouseReport &r, uint32_t &t) {
  if (!mq_n_) return false;
  MEnt &h = mq_[mq_head_];
  const int32_t x = clampTo(h.x, 32767), y = clampTo(h.y, 32767);
  const int32_t w = clampTo(h.wheel, 127), p = clampTo(h.pan, 127);
  r.buttons = h.buttons;
  r.x = (int16_t)x;
  r.y = (int16_t)y;
  r.wheel = (int8_t)w;
  r.pan = (int8_t)p;
  t = h.t;
  h.x -= x;
  h.y -= y;
  h.wheel -= w;
  h.pan -= p;
  if (!h.x && !h.y && !h.wheel && !h.pan) {   // fully delivered
    mq_head_ = (mq_head_ + 1) % kMouseQ;
    mq_n_--;
  }
  btn_sent_ = r.buttons;
  return true;
}

// ------------------------------------------------------------------ consumer
void KVM_HOT(HidOut::pushConsumer)(uint16_t u, uint32_t t) {
  if (u == cons_last_) return;
  cons_last_ = u;
  if (cq_n_ == kConsQ) {
    cq_[(cq_head_ + cq_n_ - 1) % kConsQ].u = u;
    return;
  }
  cq_[(cq_head_ + cq_n_) % kConsQ] = {u, t};
  cq_n_++;
}

bool KVM_HOT(HidOut::popConsumer)(uint16_t &u, uint32_t &t) {
  if (!cq_n_) return false;
  u = cq_[cq_head_].u;
  t = cq_[cq_head_].t;
  cq_head_ = (cq_head_ + 1) % kConsQ;
  cq_n_--;
  cons_sent_ = u;
  return true;
}

// ------------------------------------------------------------------ switch / link loss
void HidOut::releaseAll(uint32_t t) {
  kq_n_ = mq_n_ = cq_n_ = 0;
  // The PC's view is what was last popped; anything queued was never seen.
  kbd_last_ = kbd_sent_;
  btn_last_ = btn_sent_;
  cons_last_ = cons_sent_;
  pushKbd(KbdReport{}, t);
  pushMouse(0, 0, 0, 0, 0, t);
  pushConsumer(0, t);
}

}  // namespace kvm
