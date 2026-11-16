#include "link_proto.h"
#include "kvm_hot.h"
#include <string.h>

namespace kvm {
namespace link {

namespace {
struct CrcTable {
  uint8_t t[256];
  constexpr CrcTable() : t() {
    for (int i = 0; i < 256; ++i) {
      uint8_t c = (uint8_t)i;
      for (int b = 0; b < 8; ++b) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : (c << 1));
      t[i] = c;
    }
  }
};
constexpr CrcTable kCrc;
}  // namespace

uint8_t KVM_HOT(crc8)(const uint8_t *p, size_t n, uint8_t crc) {
  while (n--) crc = kCrc.t[crc ^ *p++];
  return crc;
}

size_t KVM_HOT(cobsEncode)(const uint8_t *in, size_t n, uint8_t *out) {
  size_t code_pos = 0, o = 1;
  uint8_t code = 1;
  for (size_t i = 0; i < n; ++i) {
    if (in[i] == 0) {
      out[code_pos] = code;
      code_pos = o++;
      code = 1;
    } else {
      out[o++] = in[i];
      if (++code == 0xFF) {
        out[code_pos] = code;
        code_pos = o++;
        code = 1;
      }
    }
  }
  out[code_pos] = code;
  return o;
}

size_t KVM_HOT(cobsDecode)(const uint8_t *in, size_t n, uint8_t *out, size_t out_cap) {
  size_t i = 0, o = 0;
  while (i < n) {
    const uint8_t code = in[i++];
    if (code == 0) return 0;
    if (i + code - 1 > n) return 0;
    for (uint8_t k = 1; k < code; ++k) {
      if (in[i] == 0 || o >= out_cap) return 0;
      out[o++] = in[i++];
    }
    if (code != 0xFF && i < n) {
      if (o >= out_cap) return 0;
      out[o++] = 0;
    }
  }
  return o;
}

size_t KVM_HOT(buildFrame)(uint8_t type, const void *payload, size_t len, uint8_t *wire) {
  if (len > kMaxPayload) return 0;
  uint8_t raw[kMaxFrame];
  raw[0] = type;
  if (len) memcpy(raw + 1, payload, len);
  raw[1 + len] = crc8(raw, 1 + len);
  size_t n = cobsEncode(raw, len + 2, wire);
  wire[n++] = 0x00;
  return n;
}

bool KVM_HOT(Receiver::feed)(uint8_t b) {
  if (b != 0x00) {
    if (n_ < sizeof buf_) buf_[n_++] = b;
    else overflow_ = true;
    return false;
  }
  // delimiter
  const size_t n = n_;
  const bool ovf = overflow_;
  n_ = 0;
  overflow_ = false;
  flen_ = 0;
  if (n == 0) return false;   // idle / back-to-back delimiters
  if (ovf) { frame_err_++; return false; }
  const size_t d = cobsDecode(buf_, n, frame_, sizeof frame_);
  if (d < 2 || d > kMaxFrame) { frame_err_++; return false; }
  if (crc8(frame_, d - 1) != frame_[d - 1]) { crc_err_++; return false; }
  flen_ = d;
  frames_++;
  return true;
}

}  // namespace link
}  // namespace kvm
