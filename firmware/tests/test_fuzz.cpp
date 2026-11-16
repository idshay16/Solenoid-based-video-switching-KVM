// Fuzz-style robustness tests (run under ASan/UBSan): odd or hostile USB
// devices and line noise must never crash, hang or read out of bounds.
#include "test.h"
#include "hid_parser.h"
#include "input_model.h"
#include "hid_out.h"
#include "link_proto.h"
#include "hid_reports.h"
#include <stdlib.h>
#include <string.h>

using namespace kvm;

static uint32_t g_rng = 12345;
static uint32_t rnd() {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 17;
  g_rng ^= g_rng << 5;
  return g_rng;
}

TEST(fuzz_parser_random_descriptors) {
  uint8_t desc[300], rep[70];
  hid::Layout l;
  hid::Decoded d;
  InputModel m;
  int parsed_fields = 0;
  // Mutation fuzzing: start from real descriptors, corrupt a few bytes.
  static const uint8_t seed_mouse[] = {KVM_HID_DESC_MOUSE16};
  static const uint8_t seed_kbd[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x19, 0x00, 0x29, 0x77, 0x95, 0x78, 0x81, 0x02,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x95, 0x05, 0x91, 0x02, 0x95, 0x03, 0x91, 0x01, 0xc0,
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x02, 0x15, 0x00, 0x26, 0x3c, 0x02, 0x19, 0x00, 0x2a,
    0x3c, 0x02, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00, 0xc0};
  for (int iter = 0; iter < 20000; ++iter) {
    const bool mouse = rnd() & 1;
    uint16_t len = mouse ? sizeof seed_mouse : sizeof seed_kbd;
    memcpy(desc, mouse ? seed_mouse : seed_kbd, len);
    for (uint32_t k = rnd() % 5; k; --k) {
      switch (rnd() % 4) {
        case 0: desc[rnd() % len] = (uint8_t)rnd(); break;                  // random byte
        case 1: desc[rnd() % len] ^= (uint8_t)(1u << (rnd() % 8)); break;   // bit flip
        case 2: len = (uint16_t)(rnd() % len + 1); break;                     // truncate
        case 3: if (len < sizeof desc) desc[len++] = (uint8_t)rnd(); break;   // append
      }
    }
    hid::parse(desc, len, l);
    CHECK(l.n <= hid::kMaxFields);
    parsed_fields += l.n;
    for (int r = 0; r < 4; ++r) {
      const uint16_t rl = rnd() % sizeof rep;
      for (uint16_t i = 0; i < rl; ++i) rep[i] = (uint8_t)rnd();
      hid::decode(l, rep, rl, d);
      m.apply(rnd() % 20, rep[0], d);   // out-of-range sources must be ignored safely
    }
  }
  CHECK(parsed_fields > 1000);   // the fuzzer actually produced fields
  m.dropSource(3);
  m.suppressHeld();
}

TEST(fuzz_link_receiver_noise) {
  link::Receiver r;
  int frames = 0;
  for (int i = 0; i < 2000000; ++i) frames += r.feed((uint8_t)((rnd() % 8) ? rnd() : 0));
  // random noise passing an 8-bit CRC is possible but rare (~1/256 of decodable frames)
  CHECK(r.crcErrors() + r.framingErrors() > (uint32_t)frames * 50);
  // still decodes a real frame afterwards
  uint8_t wire[link::kMaxWire];
  const size_t n = link::buildFrame(link::RELEASE_ALL, nullptr, 0, wire);
  r.feed(0);
  int got = 0;
  for (size_t i = 0; i < n; ++i) got += r.feed(wire[i]);
  CHECK_EQ(got, 1);
}

TEST(fuzz_hidout_random_ops) {
  HidOut o;
  KbdReport k;
  MouseReport mr;
  uint16_t c;
  uint32_t t;
  int64_t sx_in = 0, sx_out = 0;
  for (int i = 0; i < 200000; ++i) {
    switch (rnd() % 7) {
      case 0: memset(&k, 0, sizeof k); k.keys[0] = rnd() % 3; o.pushKbd(k, i); break;
      case 1: { int32_t dx = (int32_t)(rnd() % 2001) - 1000; sx_in += dx; o.pushMouse(rnd() % 2, dx, 0, 0, 0, i); break; }
      case 2: o.pushConsumer(rnd() % 2, i); break;
      case 3: o.popKbd(k, t); break;
      case 4: if (o.popMouse(mr, t)) sx_out += mr.x; break;
      case 5: o.popConsumer(c, t); break;
      case 6: if (rnd() % 1000 == 0) { while (o.popMouse(mr, t)) sx_out += mr.x; sx_in = sx_out; o.releaseAll(i); } break;
    }
  }
  while (o.popMouse(mr, t)) sx_out += mr.x;
  CHECK_EQ(sx_out, sx_in);   // coalescing never loses or invents motion
}
