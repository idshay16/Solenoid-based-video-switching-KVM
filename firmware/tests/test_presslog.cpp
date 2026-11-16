// Press log tests with a simulated NOR flash (program only clears bits,
// erase sets 0xFF) including power cuts in the middle of a page program.
#include "test.h"
#include "presslog.h"
#include <string.h>
#include <vector>

using namespace kvm;

struct SimFlash : FlashIf {
  std::vector<uint8_t> mem;
  int erases = 0, programs = 0;
  int cut_after_bytes = -1;   // >= 0: next program stops after this many bytes of the record area
  explicit SimFlash(uint32_t sz = 65536) : mem(sz, 0xFF) {}
  uint32_t size() const override { return (uint32_t)mem.size(); }
  const uint8_t *data() const override { return mem.data(); }
  void program256(uint32_t off, const uint8_t *page) override {
    programs++;
    int limit = 256;
    if (cut_after_bytes >= 0) {
      // find the first non-FF byte (the record) and stop part-way through it
      int first = 0;
      while (first < 256 && page[first] == 0xFF) first++;
      limit = first + cut_after_bytes;
      cut_after_bytes = -1;
    }
    for (int i = 0; i < 256 && i < limit; ++i) mem[off + i] &= page[i];   // NOR: 1 -> 0 only
  }
  void erase4k(uint32_t off) override {
    erases++;
    memset(&mem[off], 0xFF, 4096);
  }
};

TEST(presslog_empty_then_append_and_remount) {
  SimFlash f;
  PressLog log(f);
  CHECK(!log.mount());
  CHECK_EQ(f.erases, 0);   // blank flash needs no erase
  CHECK(log.append(PressLog::BOOT, 0, 0, 0));
  CHECK(log.append(PressLog::PRESS, 3, 1, 3));
  CHECK(log.append(PressLog::NUDGE, 2, 1, 1));

  PressLog again(f);
  CHECK(again.mount());
  CHECK_EQ(again.latest().type, PressLog::NUDGE);
  CHECK_EQ(again.latest().dp, 2);
  CHECK_EQ(again.latest().target, 1);
  CHECK_EQ(again.latest().seq, 3);
  CHECK_EQ(again.latest().presses, 2);
  CHECK_EQ(again.used(), 3);
}

TEST(presslog_torn_write_keeps_previous_state) {
  for (int cut = 0; cut < 16; ++cut) {
    SimFlash f;
    PressLog log(f);
    log.mount();
    log.append(PressLog::PRESS, 3, 1, 3);
    f.cut_after_bytes = cut;   // power dies while programming the next record
    log.append(PressLog::PRESS, 0, 0, 3);

    PressLog after(f);
    CHECK(after.mount());
    // either the torn record is unreadable (state = previous) or it was cut
    // after the last byte that matters... a cut < 16 means CRC can't be complete
    CHECK_EQ(after.latest().dp, 3);
    CHECK_EQ(after.latest().seq, 1);
    // and the next append goes after the torn slot, never on top of it
    CHECK(after.append(PressLog::PRESS, 0, 0, 3));
    PressLog third(f);
    CHECK(third.mount());
    CHECK_EQ(third.latest().dp, 0);
    CHECK_EQ(third.latest().seq, 2);
  }
}

TEST(presslog_compacts_when_half_full) {
  SimFlash f;
  PressLog log(f);
  log.mount();
  const uint32_t half = log.capacity() / 2;
  for (uint32_t i = 0; i <= half; ++i) log.append(PressLog::PRESS, i & 3, i & 1, 3);
  const PressRecord before = log.latest();
  PressLog boot(f);
  CHECK(boot.mount());
  CHECK(boot.compacted());
  CHECK_EQ(boot.used(), 1);
  CHECK_EQ(boot.latest().seq, before.seq);
  CHECK_EQ(boot.latest().dp, before.dp);
  CHECK_EQ(f.erases, 9);   // only sectors that held data (2049 records = 9 sectors)
}

TEST(presslog_full_keeps_ram_state) {
  SimFlash f(4096);   // tiny region: 256 records
  PressLog log(f);
  log.mount();
  for (int i = 0; i < 256; ++i) CHECK(log.append(PressLog::PRESS, 1, 0, 1));
  CHECK(!log.append(PressLog::PRESS, 2, 1, 3));   // full
  CHECK_EQ(log.latest().dp, 2);                     // RAM still right
}

TEST(presslog_foreign_data_is_wiped) {
  SimFlash f;
  memset(&f.mem[0], 0x12, 5000);   // e.g. an old LittleFS image
  PressLog log(f);
  CHECK(!log.mount());
  CHECK(log.compacted());
  CHECK_EQ(log.used(), 0);
  CHECK(log.append(PressLog::BOOT, 0, 0, 0));
  PressLog again(f);
  CHECK(again.mount());
}
