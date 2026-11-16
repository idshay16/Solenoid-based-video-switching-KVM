#include "presslog.h"
#include "link_proto.h"   // crc8
#include <string.h>

namespace kvm {

namespace {
bool blank(const uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (p[i] != 0xFF) return false;
  return true;
}
}  // namespace

bool PressLog::valid(const PressRecord &r) {
  return r.magic == kMagic && link::crc8((const uint8_t *)&r, sizeof r - 1) == r.crc;
}

bool PressLog::record(uint32_t i, PressRecord &out) const {
  if (i >= capacity()) return false;
  memcpy(&out, f_.data() + i * sizeof(PressRecord), sizeof out);
  return valid(out);
}

bool PressLog::mount() {
  have_ = false;
  corrupt_ = 0;
  compacted_ = false;
  next_ = 0;
  const uint32_t cap = capacity();
  int64_t last_used = -1;
  for (uint32_t i = 0; i < cap; ++i) {
    const uint8_t *p = f_.data() + i * sizeof(PressRecord);
    if (blank(p, sizeof(PressRecord))) continue;
    last_used = i;
    PressRecord r;
    memcpy(&r, p, sizeof r);
    if (!valid(r)) { corrupt_++; continue; }
    if (!have_ || (int32_t)(r.seq - last_.seq) > 0) {
      last_ = r;
      have_ = true;
    }
  }
  next_ = (uint32_t)(last_used + 1);
  // Free space after the last used slot must really be erased; anything else
  // (foreign data, e.g. an old LittleFS image) means: start clean.
  bool tail_clean = blank(f_.data() + next_ * sizeof(PressRecord), (cap - next_) * sizeof(PressRecord));
  if (!tail_clean || next_ > cap / 2 || (!have_ && next_ > 0)) compact();
  return have_;
}

void PressLog::compact() {
  for (uint32_t off = 0; off < f_.size(); off += 4096) {
    if (!blank(f_.data() + off, 4096)) f_.erase4k(off);
  }
  next_ = 0;
  compacted_ = true;
  if (have_) write(last_);   // same seq: it's the same state, just moved
}

bool PressLog::write(const PressRecord &r) {
  if (next_ >= capacity()) return false;
  const uint32_t off = next_ * sizeof(PressRecord);
  uint8_t page[256];
  memset(page, 0xFF, sizeof page);   // programming 0xFF leaves existing bits alone
  memcpy(page + (off & 255), &r, sizeof r);
  f_.program256(off & ~255u, page);
  next_++;
  return true;
}

bool PressLog::append(Type type, uint8_t dp, uint8_t target, uint8_t mask) {
  PressRecord r = {};
  r.magic = kMagic;
  r.type = type;
  r.dp = dp & 3;
  r.seq = have_ ? last_.seq + 1 : 1;
  r.presses = (have_ ? last_.presses : 0) + ((type == PRESS || type == NUDGE) ? 1 : 0);
  r.target = target;
  r.mask = mask;
  r.reserved = 0xFF;
  r.crc = link::crc8((const uint8_t *)&r, sizeof r - 1);
  last_ = r;          // RAM state follows even if flash is full
  have_ = true;
  return write(r);
}

}  // namespace kvm
