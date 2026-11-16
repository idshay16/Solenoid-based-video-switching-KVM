#include "switch_logic.h"

namespace kvm {

bool SwitchLogic::busy(uint32_t now) const { return active_ || (have_lockout_ && !reached(now, lockout_end_)); }

void SwitchLogic::start(uint8_t mask, bool input, uint32_t now) {
  active_ = true;
  t0_ = now;
  mask_ = mask;
  input_ = input;
  on_done_ = off_done_ = 0;
  input_done_ = !input;
  if (input) target_ ^= 1;
  uint32_t lock = cfg_.lockout_ms;
  const uint32_t min_lock = cfg_.pulse_ms + cfg_.stagger_ms;
  if (lock < min_lock) lock = min_lock;
  if (input && lock < cfg_.input_delay_ms) lock = cfg_.input_delay_ms;
  lockout_end_ = now + lock;
  have_lockout_ = true;
}

bool SwitchLogic::press(uint32_t now) {
  if (busy(now)) {
    pending_ = true;   // collapse: any number of extra presses = one more toggle
    return false;
  }
  start(RELAY_A | RELAY_B, true, now);
  return true;
}

bool SwitchLogic::nudge(uint8_t mask, uint32_t now) {
  mask &= RELAY_A | RELAY_B;
  if (!mask || busy(now)) return false;
  start(mask, false, now);
  return true;
}

SwitchLogic::Actions SwitchLogic::tick(uint32_t now) {
  Actions a;
  if (!active_) {
    if (pending_ && !busy(now)) {
      pending_ = false;
      start(RELAY_A | RELAY_B, true, now);
    } else {
      return a;
    }
  }
  const uint32_t tA = t0_, tB = t0_ + cfg_.stagger_ms;
  if ((mask_ & RELAY_A) && !(on_done_ & RELAY_A) && reached(now, tA)) { a.relay_on |= RELAY_A; on_done_ |= RELAY_A; }
  if ((mask_ & RELAY_B) && !(on_done_ & RELAY_B) && reached(now, tB)) { a.relay_on |= RELAY_B; on_done_ |= RELAY_B; }
  a.pressed = a.relay_on;
  if (!input_done_ && reached(now, t0_ + cfg_.input_delay_ms)) {
    input_done_ = true;
    a.move_input = true;
    a.target = target_;
  }
  if ((on_done_ & RELAY_A) && !(off_done_ & RELAY_A) && reached(now, tA + cfg_.pulse_ms)) { a.relay_off |= RELAY_A; off_done_ |= RELAY_A; }
  if ((on_done_ & RELAY_B) && !(off_done_ & RELAY_B) && reached(now, tB + cfg_.pulse_ms)) { a.relay_off |= RELAY_B; off_done_ |= RELAY_B; }
  if (off_done_ == mask_ && input_done_) active_ = false;
  return a;
}

}  // namespace kvm
