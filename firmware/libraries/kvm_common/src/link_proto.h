// link_proto.h — Pico A <-> Pico B UART protocol (through the ADuM1201).
// Pure C++ (host-testable).
//
// Frame on the wire:  COBS( type, payload..., crc8 )  0x00
//   * COBS removes every 0x00 from the frame, so 0x00 is an unambiguous frame
//     delimiter: after any glitch the receiver resyncs at the next 0x00.
//   * CRC-8 (poly 0x07) over type+payload rejects corrupted frames.
// Max decoded frame: kMaxPayload + 2 bytes.
//
// State is self-healing: A's heartbeat (every KVM_HEARTBEAT_MS) carries the full
// held keyboard/button/media state, so one lost frame (e.g. a button release)
// is corrected within one heartbeat instead of leaving something stuck.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace kvm {
namespace link {

constexpr uint8_t kVersion = 1;
constexpr size_t kMaxPayload = 48;
constexpr size_t kMaxFrame = kMaxPayload + 2;                  // type + payload + crc
constexpr size_t kMaxWire = kMaxFrame + kMaxFrame / 254 + 2;   // COBS overhead + delimiter

enum Type : uint8_t {
  // A -> B
  KBD = 0x01,         // KbdReport (8)
  MOUSE = 0x02,       // buttons u8, dx i16, dy i16, wheel i8, pan i8 (7) — motion is a delta
  CONS = 0x03,        // usage u16
  RELEASE_ALL = 0x04, // -
  HEARTBEAT = 0x05,   // Heartbeat
  PING = 0x06,        // seq u16, payload...
  SET_BAUD = 0x07,    // baud u32 (probe only)
  // B -> A
  ACK = 0x81,         // Ack
  LEDS = 0x82,        // leds u8 (PC B changed its lock LEDs)
  PONG = 0x83,        // echo of PING
  HELLO = 0x84,       // version u8 (B just booted)
  POLL = 0x85,        // PollTiming (SOF phase lock, step 10)
  BAUD_OK = 0x86,     // baud u32 (probe only)
};

struct __attribute__((packed)) Heartbeat {
  uint8_t  seq;
  uint8_t  b_active;     // 1 = B is the target PC
  uint8_t  kbd[8];       // full KbdReport B should be showing (zeros if not active)
  uint8_t  buttons;
  uint16_t cons;
};

struct __attribute__((packed)) Ack {
  uint8_t seq;
  uint8_t leds;          // PC B's lock LEDs
  uint8_t flags;         // bit0 mounted, bit1 suspended
  uint8_t rx_errors;     // B's CRC/framing errors (saturating), for link health
};

struct __attribute__((packed)) PollTiming {
  uint32_t us_since_poll;  // B: microseconds between PC B's last poll and sending this
  uint16_t period_us;      // B's measured poll period
};

// ------------------------------------------------------------------ CRC / COBS
uint8_t crc8(const uint8_t *p, size_t n, uint8_t crc = 0);

// Encode `n` bytes into `out` (needs n + n/254 + 1 bytes). Returns length (no delimiter).
size_t cobsEncode(const uint8_t *in, size_t n, uint8_t *out);
// Decode (out may equal in). Returns decoded length, 0 on error or if > out_cap.
size_t cobsDecode(const uint8_t *in, size_t n, uint8_t *out, size_t out_cap);

// Build a complete wire frame (COBS + trailing 0x00) into `wire` (>= kMaxWire).
// Returns bytes to transmit, 0 if payload too long.
size_t buildFrame(uint8_t type, const void *payload, size_t len, uint8_t *wire);

// ------------------------------------------------------------------ streaming receiver
class Receiver {
 public:
  // Feed one received byte. Returns true when a valid frame is ready; then
  // type()/payload()/len() describe it until the next feed().
  bool feed(uint8_t b);
  uint8_t type() const { return frame_[0]; }
  const uint8_t *payload() const { return frame_ + 1; }
  size_t len() const { return flen_ ? flen_ - 2 : 0; }

  uint32_t frames() const { return frames_; }
  uint32_t crcErrors() const { return crc_err_; }
  uint32_t framingErrors() const { return frame_err_; }
  void resetStats() { frames_ = crc_err_ = frame_err_ = 0; }

 private:
  uint8_t buf_[kMaxWire];
  size_t n_ = 0;
  bool overflow_ = false;
  uint8_t frame_[kMaxFrame];
  size_t flen_ = 0;
  uint32_t frames_ = 0, crc_err_ = 0, frame_err_ = 0;
};

}  // namespace link
}  // namespace kvm
