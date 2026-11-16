// presslog.h — persistent log of DP switch presses in a raw flash ring.
// Pure C++ over a tiny flash interface (host tests simulate power cuts).
//
// Why: the DP switches have no feedback; firmware tracks their input open-loop.
// Every press appends one 16-byte record holding the resulting state, so the
// state survives power loss (needed if the switches remember their input,
// KVM_DP_POWERON_REMEMBER) and the history can be inspected (console "log").
//
// Flash rules on the Pico: a page program (~1 ms) stalls both cores, so append()
// happens only at switch time; sector erases (~50 ms each) happen only in
// mount() at boot, before the USB host starts.
//   * Region: the 64 KB "FS" area (16 sectors x 256 records).
//   * Records are appended in order; a free slot reads all 0xFF.
//   * A record cut short by a power loss fails its CRC and is skipped; the
//     previous record still holds a valid state.
//   * mount() compacts (erase all, rewrite the latest state) when over half full,
//     so a session always has >= 2048 free records.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace kvm {

struct FlashIf {
  virtual ~FlashIf() {}
  virtual uint32_t size() const = 0;                         // bytes, multiple of 4096
  virtual const uint8_t *data() const = 0;                   // memory-mapped read
  virtual void program256(uint32_t off, const uint8_t *page) = 0;   // off % 256 == 0
  virtual void erase4k(uint32_t off) = 0;                    // off % 4096 == 0
};

struct __attribute__((packed)) PressRecord {
  uint16_t magic;    // 0x4B56 "VK"
  uint8_t  type;     // Type
  uint8_t  dp;       // DP state AFTER this event: bit0 = monitor A on input 2, bit1 = monitor B on input 2
  uint32_t seq;      // monotonically increasing
  uint32_t presses;  // total presses ever (both-switch toggles + nudges)
  uint8_t  target;   // input target after this event (0 = PC A, 1 = PC B)
  uint8_t  mask;     // which switches were pressed (PRESS/NUDGE)
  uint8_t  reserved;
  uint8_t  crc;      // CRC-8 over the first 15 bytes
};
static_assert(sizeof(PressRecord) == 16, "record must be 16 bytes");

class PressLog {
 public:
  enum Type : uint8_t { BOOT = 1, PRESS = 2, NUDGE = 3, BASELINE = 4 };
  static constexpr uint16_t kMagic = 0x4B56;

  explicit PressLog(FlashIf &f) : f_(f) {}

  // Scan the region. Returns true if a valid record was found (latest() valid).
  // May erase (compaction / first use).
  bool mount();
  bool hasState() const { return have_; }
  const PressRecord &latest() const { return last_; }

  // Append an event; fills seq/presses/crc. Returns false if the log is full
  // (state is still tracked in RAM; the next boot compacts).
  bool append(Type type, uint8_t dp, uint8_t target, uint8_t mask);

  uint32_t used() const { return next_; }
  uint32_t capacity() const { return f_.size() / sizeof(PressRecord); }
  uint32_t corrupt() const { return corrupt_; }
  bool compacted() const { return compacted_; }

  // Read record i (0 = oldest slot). Returns false if empty/invalid.
  bool record(uint32_t i, PressRecord &out) const;

  static bool valid(const PressRecord &r);

 private:
  void compact();
  bool write(const PressRecord &r);

  FlashIf &f_;
  PressRecord last_ = {};
  bool have_ = false;
  uint32_t next_ = 0;      // next free slot
  uint32_t corrupt_ = 0;
  bool compacted_ = false;
};

}  // namespace kvm
