// hid_out.h — per-PC output queue between "input arrived" and "PC polled us".
// Pure C++ (host-testable); the firmware wraps calls in a spinlock when the
// producer and consumer run on different cores.
//
// Latency rules from the architecture:
//  * Mouse motion is COALESCED: while the PC hasn't collected the previous report,
//    new motion is added to the waiting one, so the PC always gets the newest
//    total and we never build a backlog (a backlog = added latency).
//  * Button and key changes are EDGES and are QUEUED: a click shorter than one
//    poll still reaches the PC as press + release, never lost.
//  * Motion bigger than one report can carry (int16 / int8 wheel) stays in the
//    queue head and goes out in the next report(s).
#pragma once
#include <stdint.h>
#include "hid_reports.h"

namespace kvm {

class HidOut {
 public:
  static constexpr uint8_t kKbdQ = 16, kMouseQ = 32, kConsQ = 8;

  HidOut() { reset(); }
  void reset();

  // Producer side. `t_us` = when the input arrived (for latency stats).
  void pushKbd(const KbdReport &r, uint32_t t_us);
  void pushMouse(uint8_t buttons, int32_t dx, int32_t dy, int32_t wheel, int32_t pan, uint32_t t_us);
  void pushConsumer(uint16_t usage, uint32_t t_us);

  // Switch away / link loss: drop everything not yet sent and queue "all released"
  // for whatever the PC currently believes is held.
  void releaseAll(uint32_t t_us);

  // Consumer side (in the USB "PC collected a report" callback). The returned
  // `t_us` is the arrival time of the oldest input folded into this report.
  bool popKbd(KbdReport &r, uint32_t &t_us);
  bool popMouse(MouseReport &r, uint32_t &t_us);
  bool popConsumer(uint16_t &usage, uint32_t &t_us);

  bool kbdPending() const { return kq_n_ != 0; }
  bool mousePending() const { return mq_n_ != 0; }
  bool consPending() const { return cq_n_ != 0; }
  bool anyPending() const { return kq_n_ || mq_n_ || cq_n_; }

  // What the PC has been sent last (its current view of held keys/buttons).
  const KbdReport &kbdSent() const { return kbd_sent_; }
  uint8_t buttonsSent() const { return btn_sent_; }

 private:
  struct KEnt { KbdReport r; uint32_t t; };
  struct MEnt { uint8_t buttons; int32_t x, y, wheel, pan; uint32_t t; };
  struct CEnt { uint16_t u; uint32_t t; };

  KEnt kq_[kKbdQ];
  uint8_t kq_head_, kq_n_;
  KbdReport kbd_last_, kbd_sent_;     // newest queued / last popped

  MEnt mq_[kMouseQ];
  uint8_t mq_head_, mq_n_;
  uint8_t btn_last_, btn_sent_;

  CEnt cq_[kConsQ];
  uint8_t cq_head_, cq_n_;
  uint16_t cons_last_, cons_sent_;
};

}  // namespace kvm
