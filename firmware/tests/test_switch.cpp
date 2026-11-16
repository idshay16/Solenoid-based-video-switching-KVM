// SwitchLogic timing tests: pulse, lockout, collapse, stagger, input delay, nudge.
#include "test.h"
#include "switch_logic.h"

using namespace kvm;
using A = SwitchLogic::Actions;

// Run tick() every ms over [from, to) and collect what happened when.
struct Trace {
  int on_a = -1, on_b = -1, off_a = -1, off_b = -1, moves = 0, last_target = -1, presses = 0;
  int move_at = -1;
};
static void run(SwitchLogic &s, uint32_t from, uint32_t to, Trace &t) {
  for (uint32_t ms = from; ms < to; ++ms) {
    A a = s.tick(ms);
    if (a.relay_on & SwitchLogic::RELAY_A) t.on_a = ms;
    if (a.relay_on & SwitchLogic::RELAY_B) t.on_b = ms;
    if (a.relay_off & SwitchLogic::RELAY_A) t.off_a = ms;
    if (a.relay_off & SwitchLogic::RELAY_B) t.off_b = ms;
    if (a.pressed) t.presses++;
    if (a.move_input) { t.moves++; t.last_target = a.target; t.move_at = ms; }
  }
}

TEST(switch_basic_toggle) {
  SwitchLogic s({100, 800, 0, 0});
  CHECK(s.press(1000));
  Trace t;
  run(s, 1000, 2000, t);
  CHECK_EQ(t.on_a, 1000);
  CHECK_EQ(t.on_b, 1000);    // both relays in the same tick (one gpio_put_masked)
  CHECK_EQ(t.off_a, 1100);
  CHECK_EQ(t.off_b, 1100);
  CHECK_EQ(t.moves, 1);
  CHECK_EQ(t.move_at, 1000);  // input moves immediately
  CHECK_EQ(t.last_target, 1);
  CHECK(!s.busy(1800));
  CHECK(s.busy(1799));
}

TEST(switch_presses_collapse_during_lockout) {
  SwitchLogic s({100, 800, 0, 0});
  s.press(0);
  CHECK(!s.press(50));
  CHECK(!s.press(300));
  CHECK(!s.press(700));      // three extra presses -> exactly one more toggle
  Trace t;
  run(s, 0, 3000, t);
  CHECK_EQ(t.moves, 2);
  CHECK_EQ(t.presses, 2);
  CHECK_EQ(t.on_a, 800);      // pending toggle starts when the lockout ends
  CHECK_EQ(s.target(), 0);    // toggled twice: back on PC A
  CHECK(!s.pending());
}

TEST(switch_stagger_and_input_delay) {
  SwitchLogic s({100, 800, 250, 30});
  s.press(0);
  Trace t;
  run(s, 0, 1000, t);
  CHECK_EQ(t.on_a, 0);
  CHECK_EQ(t.on_b, 30);
  CHECK_EQ(t.off_a, 100);
  CHECK_EQ(t.off_b, 130);
  CHECK_EQ(t.move_at, 250);
}

TEST(switch_lockout_never_shorter_than_pulse) {
  SwitchLogic s({300, 50, 0, 20});
  s.press(0);
  Trace t;
  run(s, 0, 320, t);          // ticks 0..319: relay B (staggered) still on
  CHECK(s.busy(319));
  run(s, 320, 321, t);        // relay B off at 320
  CHECK_EQ(t.off_b, 320);
  CHECK(!s.busy(320));
}

TEST(switch_nudge_one_relay_no_input) {
  SwitchLogic s({100, 800, 0, 0});
  CHECK(s.nudge(SwitchLogic::RELAY_B, 0));
  Trace t;
  run(s, 0, 1000, t);
  CHECK_EQ(t.on_a, -1);
  CHECK_EQ(t.on_b, 0);
  CHECK_EQ(t.off_b, 100);
  CHECK_EQ(t.moves, 0);
  CHECK_EQ(s.target(), 0);
  CHECK(!s.nudge(SwitchLogic::RELAY_A, 500));   // busy during lockout
  CHECK(s.nudge(SwitchLogic::RELAY_A, 800));
}

TEST(switch_millis_wraparound) {
  SwitchLogic s({100, 800, 0, 0});
  const uint32_t t0 = 0xFFFFFFFFu - 50;
  s.press(t0);
  Trace t;
  for (uint32_t i = 0; i < 1000; ++i) {
    A a = s.tick(t0 + i);
    if (a.relay_off & SwitchLogic::RELAY_A) t.off_a = (int)i;
  }
  CHECK_EQ(t.off_a, 100);
  CHECK(!s.busy(t0 + 800));
}
