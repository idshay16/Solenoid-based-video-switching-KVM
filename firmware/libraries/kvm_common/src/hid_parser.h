// hid_parser.h — our own HID report-descriptor parser + report decoder.
// Pure C++ (no Arduino / TinyUSB), so firmware/tests can run it on the PC.
//
// parse() walks a report descriptor once (at mount time) and produces a flat
// list of Fields: "bits 16..31 of report ID 2 are mouse X, signed". decode()
// then turns each incoming report into a Decoded struct with a fixed meaning
// (keyboard bitmap + modifiers, mouse buttons/motion, consumer usage) no matter
// how the device laid its bytes out. This is how 16-bit / 12-bit / 8-bit mice,
// 6KRO / NKRO keyboards and media keys all become the same thing.
//
// Only what a KVM needs is extracted: keyboard (page 0x07), mouse buttons and
// relative X/Y/wheel/pan inside a Mouse/Pointer application collection, consumer
// usages (page 0x0C) inside a Consumer Control collection, and the keyboard LED
// output report. Everything else (vendor pages, gamepads, absolute pointers,
// feature reports) is ignored.
#pragma once
#include <stdint.h>

namespace kvm {
namespace hid {

enum class Kind : uint8_t {
  ModBits,     // 1-bit variables, usages 0xE0..0xE7 (modifier keys)
  KeyBitmap,   // 1-bit variables, one per key usage (NKRO)
  KeyArray,    // array of key usages (6KRO style)
  Buttons,     // 1-bit variables, mouse buttons (usage 1 = left)
  X, Y, Wheel, Pan,
  ConsArray,   // array of consumer usages
  ConsBit,     // single 1-bit consumer usage (e.g. a "Volume Up" bit)
};

struct Field {
  uint8_t  report_id;   // 0 when the device uses no report IDs
  Kind     kind;
  uint8_t  size;        // bits per element (1..32)
  uint8_t  count;       // number of elements
  uint16_t bit_off;     // offset from the first byte after the report ID
  uint16_t usage;       // first usage of the run (ModBits: 0xE0+n, Buttons: 1+n, arrays: usage at lmin)
  int32_t  lmin, lmax;  // logical range (arrays map value lmin -> usage)
  bool     is_signed;
};

constexpr int kMaxFields = 64;

struct Layout {
  Field   f[kMaxFields];
  uint8_t n = 0;
  bool    has_report_ids = false;
  bool    truncated = false;      // ran out of Field slots
  // Keyboard LED output report (where Caps/Num/Scroll go when the PC changes them).
  bool     has_leds = false;
  uint8_t  led_rid = 0;
  uint16_t led_bit_off = 0;       // bit offset of LED usage `led_first`
  uint8_t  led_first = 1;         // LED usage at led_bit_off (1 = Num Lock)
  uint8_t  led_count = 0;
  uint16_t led_report_bytes = 0;  // output report length (without ID)

  bool hasKeyboard() const;
  bool hasMouse() const;
  bool hasConsumer() const;
};

// Returns false on a malformed descriptor (layout may still be partly filled).
bool parse(const uint8_t *desc, uint16_t len, Layout &out);

// Fixed layouts for devices left in boot protocol (or with unusable descriptors).
void bootKeyboard(Layout &out);
void bootMouse(Layout &out);

// What one report said. `what` says which parts are valid for THIS report ID;
// a report that carries keyboard fields is a full keyboard snapshot.
struct Decoded {
  enum : uint8_t { KBD = 1, MOUSE = 2, CONS = 4 };
  uint8_t  what = 0;
  bool     rollover = false;     // keyboard sent ErrorRollOver: keep previous key state
  uint8_t  mods = 0;
  uint32_t keys[8] = {};         // 256-bit bitmap of pressed key usages (0x04..0xFF), mods excluded
  uint16_t buttons = 0;          // bit0 = button 1 (left)
  int32_t  x = 0, y = 0, wheel = 0, pan = 0;
  uint16_t cons = 0;             // a pressed consumer usage, 0 = none

  bool key(uint8_t u) const { return keys[u >> 5] & (1u << (u & 31)); }
};

void decode(const Layout &l, const uint8_t *report, uint16_t len, Decoded &out);

// Little-endian bit-field extraction (exposed for tests).
uint32_t extractBits(const uint8_t *data, uint16_t data_len, uint32_t bit_off, uint8_t size);

}  // namespace hid
}  // namespace kvm
