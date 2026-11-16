#include "hid_parser.h"
#include "kvm_hot.h"
#include <string.h>

namespace kvm {
namespace hid {

namespace {

constexpr uint32_t U(uint16_t page, uint16_t id) { return ((uint32_t)page << 16) | id; }

constexpr uint16_t PAGE_GD = 0x01, PAGE_KBD = 0x07, PAGE_LED = 0x08, PAGE_BTN = 0x09, PAGE_CONS = 0x0C;
constexpr uint32_t APP_MOUSE = U(PAGE_GD, 0x02), APP_POINTER = U(PAGE_GD, 0x01),
                   APP_CONSUMER = U(PAGE_CONS, 0x01);

struct Globals {
  uint16_t page = 0;
  int32_t  lmin = 0, lmax = 0;
  uint32_t lmax_u = 0;           // unsigned reading of Logical Maximum (for 0xFF-as-255 descriptors)
  uint8_t  size = 0;
  uint8_t  rid = 0;
  uint16_t count = 0;
};

struct RidBits { uint8_t rid; uint16_t in_bits, out_bits; };

struct Parser {
  Layout &L;
  Globals g;
  Globals stack[4];
  uint8_t sp = 0;

  uint32_t usages[32];
  uint8_t  n_usages = 0;
  uint32_t umin = 0, umax = 0;
  bool     has_min = false, has_max = false;

  uint32_t app_stack[16];
  uint8_t  coll_depth = 0;   // number of open collections
  uint32_t app = 0;          // innermost application collection usage

  RidBits rids[32];
  uint8_t n_rids = 0;

  explicit Parser(Layout &l) : L(l) {}

  RidBits &rid(uint8_t id) {
    for (uint8_t i = 0; i < n_rids; ++i)
      if (rids[i].rid == id) return rids[i];
    if (n_rids < 32) {
      rids[n_rids] = {id, 0, 0};
      return rids[n_rids++];
    }
    return rids[31];   // absurd descriptor; offsets will be wrong but we stay in bounds
  }

  void clearLocals() { n_usages = 0; has_min = has_max = false; umin = umax = 0; }

  // Usage for element i of a Variable main item; 0 = none.
  uint32_t usageAt(uint16_t i) const {
    if (n_usages) return usages[i < n_usages ? i : n_usages - 1];
    if (has_min) {
      uint32_t u = umin + i;
      if (has_max && u > umax) return 0;
      return u;
    }
    return 0;
  }

  int32_t effLmax() const {
    // "Logical Maximum (255)" is often encoded in one byte (0x25 0xFF) which, read
    // signed, is -1. If that makes max < min, the author meant the unsigned value.
    return g.lmax < g.lmin ? (int32_t)g.lmax_u : g.lmax;
  }

  Field *addField(Kind k, uint16_t bit_off, uint8_t size, uint16_t usage) {
    if (L.n >= kMaxFields) { L.truncated = true; return nullptr; }
    Field &f = L.f[L.n++];
    f.report_id = g.rid;
    f.kind = k;
    f.size = size;
    f.count = 1;
    f.bit_off = bit_off;
    f.usage = usage;
    f.lmin = g.lmin;
    f.lmax = effLmax();
    f.is_signed = g.lmin < 0;
    return &f;
  }

  // Extend the previous field if this element continues its run (same kind,
  // next usage, next bit position); otherwise start a new field.
  void addRunElement(Kind k, uint16_t bit_off, uint16_t usage) {
    if (L.n) {
      Field &p = L.f[L.n - 1];
      if (p.kind == k && p.report_id == g.rid && p.size == 1 && p.count < 255 &&
          p.usage + p.count == usage && p.bit_off + p.count == bit_off) {
        p.count++;
        return;
      }
    }
    addField(k, bit_off, 1, usage);
  }

  void input(uint32_t flags) {
    RidBits &rb = rid(g.rid);
    const uint16_t start = rb.in_bits;
    rb.in_bits += (uint16_t)(g.size * g.count);
    if (flags & 0x01) return;          // Constant (padding)
    if (g.size == 0 || g.size > 32) return;
    const bool variable = flags & 0x02, relative = flags & 0x04;
    const bool mouse_app = app == APP_MOUSE || app == APP_POINTER;

    if (!variable) {
      uint32_t base = has_min ? umin : (n_usages ? usages[0] : 0);
      uint16_t page = base >> 16;
      if (page == PAGE_KBD || page == PAGE_CONS) {
        Field *f = addField(page == PAGE_KBD ? Kind::KeyArray : Kind::ConsArray, start, g.size, base & 0xFFFF);
        if (f) f->count = g.count > 255 ? 255 : (uint8_t)g.count;
      }
      return;
    }

    for (uint16_t i = 0; i < g.count; ++i) {
      const uint32_t u = usageAt(i);
      const uint16_t page = u >> 16, id = u & 0xFFFF;
      const uint16_t off = start + i * g.size;
      if (!u) continue;
      if (page == PAGE_KBD && g.size == 1 && id <= 0xFF) {
        addRunElement(id >= 0xE0 && id <= 0xE7 ? Kind::ModBits : Kind::KeyBitmap, off, id);
      } else if (page == PAGE_BTN && g.size == 1 && mouse_app && id >= 1) {
        addRunElement(Kind::Buttons, off, id);
      } else if (page == PAGE_GD && relative && mouse_app && (id == 0x30 || id == 0x31 || id == 0x38)) {
        addField(id == 0x30 ? Kind::X : id == 0x31 ? Kind::Y : Kind::Wheel, off, g.size, id);
      } else if (page == PAGE_CONS && id == 0x238 && relative) {
        addField(Kind::Pan, off, g.size, id);
      } else if (page == PAGE_CONS && g.size == 1 && app == APP_CONSUMER) {
        addField(Kind::ConsBit, off, 1, id);
      }
    }
  }

  void output(uint32_t flags) {
    RidBits &rb = rid(g.rid);
    const uint16_t start = rb.out_bits;
    rb.out_bits += (uint16_t)(g.size * g.count);
    if ((flags & 0x01) || !(flags & 0x02) || g.size != 1) return;
    const uint32_t u = usageAt(0);
    if ((u >> 16) != PAGE_LED || L.has_leds) return;   // first LED block only
    L.has_leds = true;
    L.led_rid = g.rid;
    L.led_bit_off = start;
    L.led_first = u & 0xFF;
    L.led_count = g.count > 8 ? 8 : (uint8_t)g.count;
  }

  bool run(const uint8_t *d, uint16_t len) {
    uint16_t i = 0;
    bool ok = true;
    while (i < len) {
      const uint8_t p = d[i];
      if (p == 0xFE) {   // long item: 0xFE, size, tag, data...
        if (i + 1 >= len) { ok = false; break; }
        i += 3 + d[i + 1];
        continue;
      }
      uint8_t sz = p & 3;
      if (sz == 3) sz = 4;
      if (i + 1 + sz > len) { ok = false; break; }
      const uint8_t type = (p >> 2) & 3, tag = p >> 4;
      uint32_t u = 0;
      for (uint8_t b = 0; b < sz; ++b) u |= (uint32_t)d[i + 1 + b] << (8 * b);
      int32_t s = (int32_t)u;
      if (sz == 1) s = (int8_t)u;
      else if (sz == 2) s = (int16_t)u;
      i += 1 + sz;

      if (type == 1) {          // global
        switch (tag) {
          case 0: g.page = (uint16_t)u; break;
          case 1: g.lmin = s; break;
          case 2: g.lmax = s; g.lmax_u = u; break;
          case 7: g.size = (uint8_t)u; break;
          case 8: g.rid = (uint8_t)u; L.has_report_ids = true; break;
          case 9: g.count = (uint16_t)u; break;
          case 10: if (sp < 4) stack[sp++] = g; break;
          case 11: if (sp) g = stack[--sp]; break;
          default: break;
        }
      } else if (type == 2) {   // local
        const uint32_t full = sz == 4 ? u : U(g.page, (uint16_t)u);
        switch (tag) {
          case 0: if (n_usages < 32) usages[n_usages++] = full; break;
          case 1: umin = full; has_min = true; break;
          case 2: umax = full; has_max = true; break;
          default: break;
        }
      } else if (type == 0) {   // main
        switch (tag) {
          case 0x8: input(u); break;
          case 0x9: output(u); break;
          case 0xA:
            if (coll_depth < 16) app_stack[coll_depth] = app;
            coll_depth++;
            if (u == 0x01) app = n_usages ? usages[0] : umin;   // Application collection
            break;
          case 0xC:
            if (coll_depth) {
              coll_depth--;
              if (coll_depth < 16) app = app_stack[coll_depth];
            }
            break;
          default: break;   // Feature: ignored
        }
        clearLocals();
      }
    }
    if (L.has_leds) L.led_report_bytes = (rid(L.led_rid).out_bits + 7) / 8;
    return ok;
  }
};

inline void setKey(Decoded &o, uint16_t u) {
  if (u >= 0xE0 && u <= 0xE7) o.mods |= 1u << (u - 0xE0);
  else if (u > 3 && u <= 0xFF) o.keys[u >> 5] |= 1u << (u & 31);
}

inline int32_t signExtend(uint32_t v, uint8_t size) {
  if (size >= 32) return (int32_t)v;
  const uint32_t m = 1u << (size - 1);
  return (int32_t)((v ^ m) - m);
}

}  // namespace

bool Layout::hasKeyboard() const {
  for (uint8_t i = 0; i < n; ++i)
    if (f[i].kind == Kind::ModBits || f[i].kind == Kind::KeyBitmap || f[i].kind == Kind::KeyArray) return true;
  return false;
}
bool Layout::hasMouse() const {
  for (uint8_t i = 0; i < n; ++i)
    if (f[i].kind == Kind::Buttons || f[i].kind == Kind::X || f[i].kind == Kind::Y) return true;
  return false;
}
bool Layout::hasConsumer() const {
  for (uint8_t i = 0; i < n; ++i)
    if (f[i].kind == Kind::ConsArray || f[i].kind == Kind::ConsBit) return true;
  return false;
}

bool parse(const uint8_t *desc, uint16_t len, Layout &out) {
  out = Layout();
  if (!desc || !len) return false;
  Parser p(out);
  return p.run(desc, len);
}

void bootKeyboard(Layout &out) {
  out = Layout();
  out.f[0] = {0, Kind::ModBits, 1, 8, 0, 0xE0, 0, 1, false};
  out.f[1] = {0, Kind::KeyArray, 8, 6, 16, 0, 0, 255, false};
  out.n = 2;
  out.has_leds = true;
  out.led_rid = 0;
  out.led_bit_off = 0;
  out.led_first = 1;
  out.led_count = 5;
  out.led_report_bytes = 1;
}

void bootMouse(Layout &out) {
  out = Layout();
  out.f[0] = {0, Kind::Buttons, 1, 3, 0, 1, 0, 1, false};
  out.f[1] = {0, Kind::X, 8, 1, 8, 0x30, -127, 127, true};
  out.f[2] = {0, Kind::Y, 8, 1, 16, 0x31, -127, 127, true};
  out.f[3] = {0, Kind::Wheel, 8, 1, 24, 0x38, -127, 127, true};   // absent in 3-byte reports -> reads 0
  out.n = 4;
}

uint32_t KVM_HOT(extractBits)(const uint8_t *d, uint16_t n, uint32_t off, uint8_t size) {
  uint32_t byte = off >> 3, shift = off & 7;
  uint64_t acc = 0;
  for (uint32_t b = 0; b < 5; ++b)
    if (byte + b < n) acc |= (uint64_t)d[byte + b] << (8 * b);
  acc >>= shift;
  return size >= 32 ? (uint32_t)acc : (uint32_t)(acc & ((1ull << size) - 1));
}

void KVM_HOT(decode)(const Layout &l, const uint8_t *r, uint16_t len, Decoded &o) {
  o = Decoded();
  uint8_t rid = 0;
  if (l.has_report_ids) {
    if (!len) return;
    rid = r[0];
    ++r;
    --len;
  }
  for (uint8_t fi = 0; fi < l.n; ++fi) {
    const Field &f = l.f[fi];
    if (f.report_id != rid) continue;
    switch (f.kind) {
      case Kind::ModBits:
        o.what |= Decoded::KBD;
        for (uint8_t i = 0; i < f.count; ++i)
          if (extractBits(r, len, f.bit_off + i, 1)) setKey(o, f.usage + i);
        break;
      case Kind::KeyBitmap:
        o.what |= Decoded::KBD;
        for (uint8_t i = 0; i < f.count; ++i)
          if (extractBits(r, len, f.bit_off + i, 1)) setKey(o, f.usage + i);
        break;
      case Kind::KeyArray:
        o.what |= Decoded::KBD;
        for (uint8_t i = 0; i < f.count; ++i) {
          uint32_t raw = extractBits(r, len, f.bit_off + i * f.size, f.size);
          int32_t v = f.is_signed ? signExtend(raw, f.size) : (int32_t)raw;
          if (v < f.lmin || v > f.lmax) continue;
          uint32_t u = f.usage + (uint32_t)(v - f.lmin);
          if (u == 0) continue;
          if (u <= 3) { o.rollover = true; continue; }   // ErrorRollOver / POSTFail / Undefined
          setKey(o, (uint16_t)u);
        }
        break;
      case Kind::Buttons:
        o.what |= Decoded::MOUSE;
        for (uint8_t i = 0; i < f.count; ++i) {
          uint16_t b = f.usage + i - 1;
          if (b < 16 && extractBits(r, len, f.bit_off + i, 1)) o.buttons |= 1u << b;
        }
        break;
      case Kind::X: case Kind::Y: case Kind::Wheel: case Kind::Pan: {
        o.what |= Decoded::MOUSE;
        uint32_t raw = extractBits(r, len, f.bit_off, f.size);
        int32_t v = f.is_signed ? signExtend(raw, f.size) : (int32_t)raw;
        if (f.kind == Kind::X) o.x += v;
        else if (f.kind == Kind::Y) o.y += v;
        else if (f.kind == Kind::Wheel) o.wheel += v;
        else o.pan += v;
        break;
      }
      case Kind::ConsArray:
        o.what |= Decoded::CONS;
        for (uint8_t i = 0; i < f.count; ++i) {
          uint32_t raw = extractBits(r, len, f.bit_off + i * f.size, f.size);
          int32_t v = f.is_signed ? signExtend(raw, f.size) : (int32_t)raw;
          if (v < f.lmin || v > f.lmax) continue;
          uint32_t u = f.usage + (uint32_t)(v - f.lmin);
          if (u && !o.cons) o.cons = (uint16_t)u;
        }
        break;
      case Kind::ConsBit:
        o.what |= Decoded::CONS;
        if (!o.cons && extractBits(r, len, f.bit_off, 1)) o.cons = f.usage;
        break;
    }
  }
}

}  // namespace hid
}  // namespace kvm
