// Link protocol tests: COBS, CRC, framing, resync after line noise.
#include "test.h"
#include "link_proto.h"
#include <string.h>
#include <stdlib.h>

using namespace kvm::link;

static int feedAll(Receiver &r, const uint8_t *w, size_t n) {
  int frames = 0;
  for (size_t i = 0; i < n; ++i) frames += r.feed(w[i]);
  return frames;
}

TEST(crc8_known_vector) {
  const uint8_t s[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  CHECK_EQ(crc8(s, 9), 0xF4);   // CRC-8/SMBUS check value
}

TEST(cobs_roundtrip_all_lengths) {
  uint8_t in[300], enc[310], dec[310];
  srand(1);
  for (size_t n = 1; n < 300; ++n) {
    for (size_t i = 0; i < n; ++i) in[i] = (rand() % 4 == 0) ? 0 : (uint8_t)rand();
    size_t e = cobsEncode(in, n, enc);
    for (size_t i = 0; i < e; ++i) CHECK(enc[i] != 0);
    size_t d = cobsDecode(enc, e, dec, sizeof dec);
    CHECK_EQ(d, n);
    CHECK(memcmp(in, dec, n) == 0);
  }
  // long run without zeros (forces 0xFF blocks)
  memset(in, 0x55, 300);
  size_t e = cobsEncode(in, 300, enc);
  CHECK_EQ(cobsDecode(enc, e, dec, sizeof dec), 300);
  CHECK_EQ(cobsDecode(enc, e, dec, 100), 0);   // output bound respected
}

TEST(frame_roundtrip_every_type) {
  Receiver r;
  uint8_t wire[kMaxWire];
  Heartbeat hb{7, 1, {0x02, 0, 0x04, 0, 0, 0, 0, 0}, 0x01, 0x00E9};
  size_t n = buildFrame(HEARTBEAT, &hb, sizeof hb, wire);
  CHECK(n > sizeof hb);
  CHECK_EQ(wire[n - 1], 0);
  CHECK_EQ(feedAll(r, wire, n), 1);
  CHECK_EQ(r.type(), HEARTBEAT);
  CHECK_EQ(r.len(), sizeof hb);
  Heartbeat got;
  memcpy(&got, r.payload(), sizeof got);
  CHECK_EQ(got.seq, 7);
  CHECK_EQ(got.cons, 0xE9);
  CHECK_EQ(got.kbd[2], 0x04);

  n = buildFrame(RELEASE_ALL, nullptr, 0, wire);
  CHECK_EQ(feedAll(r, wire, n), 1);
  CHECK_EQ(r.type(), RELEASE_ALL);
  CHECK_EQ(r.len(), 0);

  uint8_t big[kMaxPayload];
  for (size_t i = 0; i < sizeof big; ++i) big[i] = (uint8_t)i;   // includes a zero byte
  n = buildFrame(PING, big, sizeof big, wire);
  CHECK(n <= kMaxWire);
  CHECK_EQ(feedAll(r, wire, n), 1);
  CHECK_EQ(r.len(), kMaxPayload);
  CHECK(memcmp(r.payload(), big, sizeof big) == 0);
  CHECK_EQ(buildFrame(PING, big, kMaxPayload + 1, wire), 0);
}

TEST(receiver_rejects_corruption_and_resyncs) {
  Receiver r;
  uint8_t wire[kMaxWire];
  const uint8_t m[7] = {1, 2, 0, 3, 4, 5, 6};
  size_t n = buildFrame(MOUSE, m, sizeof m, wire);

  // flip one bit: CRC (or COBS) must reject it
  uint8_t bad[kMaxWire];
  int rejected = 0;
  for (size_t i = 0; i + 1 < n; ++i) {
    for (int b = 0; b < 8; ++b) {
      memcpy(bad, wire, n);
      bad[i] ^= (uint8_t)(1 << b);
      Receiver rr;
      if (bad[i] == 0) continue;   // turned into a delimiter: splits the frame, also rejected below
      rejected += feedAll(rr, bad, n) == 0;
    }
  }
  CHECK(rejected > 0);
  int accepted_bad = 0;
  for (size_t i = 0; i + 1 < n; ++i)
    for (int b = 0; b < 8; ++b) {
      memcpy(bad, wire, n);
      bad[i] ^= (uint8_t)(1 << b);
      Receiver rr;
      accepted_bad += feedAll(rr, bad, n);
    }
  CHECK_EQ(accepted_bad, 0);   // CRC-8 catches every single-bit error

  // garbage, then a good frame: receiver resyncs at the delimiter
  const uint8_t junk[] = {0x13, 0x77, 0xFF, 0x00, 0x42, 0x42};
  feedAll(r, junk, sizeof junk);
  uint8_t z = 0;
  feedAll(r, &z, 1);
  CHECK_EQ(feedAll(r, wire, n), 1);
  CHECK_EQ(r.type(), MOUSE);
  CHECK(r.framingErrors() + r.crcErrors() >= 1);

  // overflow: endless non-zero bytes then a good frame
  Receiver o;
  for (int i = 0; i < 500; ++i) o.feed(0x5A);
  o.feed(0);
  CHECK_EQ(o.framingErrors(), 1);
  CHECK_EQ(feedAll(o, wire, n), 1);
}

TEST(back_to_back_frames) {
  Receiver r;
  uint8_t wire[3 * kMaxWire];
  size_t n = 0;
  const uint16_t c1 = 0xE9, c2 = 0;
  n += buildFrame(CONS, &c1, 2, wire + n);
  n += buildFrame(CONS, &c2, 2, wire + n);
  n += buildFrame(RELEASE_ALL, nullptr, 0, wire + n);
  CHECK_EQ(feedAll(r, wire, n), 3);
  CHECK_EQ(r.type(), RELEASE_ALL);
  CHECK_EQ(r.frames(), 3);
}
