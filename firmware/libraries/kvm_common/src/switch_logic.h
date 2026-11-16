// switch_logic.h — timing of one KVM switch: relays, input move, lockout.
// Pure C++ (host-testable); the firmware calls press()/tick() and performs the
// returned Actions (GPIO writes, router target change, press log).
//
// One toggle at t0:
//   t0                       relay A on (both relays together unless stagger > 0)
//   t0 + stagger             relay B on
//   t0 + input_delay         keyboard/mouse move to the other PC (default 0 = at t0)
//   t0 + pulse (+stagger)    relays off
//   t0 + lockout             ready for the next toggle
// Presses during a toggle or its lockout collapse into ONE pending toggle that
// starts when the lockout ends (video and input always move together, so the
// DP switches can't end up out of step with where the keyboard goes).
// nudge(mask) presses one switch only (repair a monitor that drifted), no input move.
#pragma once
#include <stdint.h>

namespace kvm {

class SwitchLogic {
 public:
  struct Config {
    uint32_t pulse_ms = 100, lockout_ms = 800, input_delay_ms = 0, stagger_ms = 0;
  };
  enum : uint8_t { RELAY_A = 1, RELAY_B = 2 };

  struct Actions {
    uint8_t relay_on = 0;    // RELAY_* bits to energise now
    uint8_t relay_off = 0;   // RELAY_* bits to release now
    uint8_t pressed = 0;     // RELAY_* bits whose DP switch just got pressed (state toggles, log it)
    bool    move_input = false;
    uint8_t target = 0;      // when move_input: 0 = PC A, 1 = PC B
    bool any() const { return relay_on || relay_off || pressed || move_input; }
  };

  explicit SwitchLogic(const Config &c) : cfg_(c) {}

  // Switch request (button edge, hotkey, console). Returns false if it was
  // collapsed into a pending toggle.
  bool press(uint32_t now_ms);
  // Press one DP switch only. Returns false if busy (try again after lockout).
  bool nudge(uint8_t mask, uint32_t now_ms);
  // Call often (every loop). Returns what to do right now.
  Actions tick(uint32_t now_ms);

  uint8_t target() const { return target_; }       // where input is / is going
  void setTarget(uint8_t t) { target_ = t; }        // boot: initial target
  bool busy(uint32_t now_ms) const;                 // in a toggle or lockout
  bool pending() const { return pending_; }

 private:
  static bool reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }
  void start(uint8_t mask, bool input, uint32_t now);

  Config cfg_;
  uint8_t target_ = 0;
  bool pending_ = false;
  bool active_ = false;
  bool have_lockout_ = false;
  uint32_t lockout_end_ = 0;
  // current job
  uint32_t t0_ = 0;
  uint8_t mask_ = 0;
  bool input_ = false;
  uint8_t on_done_ = 0, off_done_ = 0;
  bool input_done_ = false;
};

}  // namespace kvm
