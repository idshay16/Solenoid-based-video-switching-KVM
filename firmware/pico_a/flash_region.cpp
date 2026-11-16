#include "flash_region.h"
#include <Arduino.h>
#include <hardware/flash.h>

extern "C" uint8_t _FS_start;
extern "C" uint8_t _FS_end;

namespace {
struct PicoFlash : kvm::FlashIf {
  uint32_t size() const override { return (uint32_t)(&_FS_end - &_FS_start) & ~4095u; }
  const uint8_t *data() const override { return &_FS_start; }
  uint32_t flashOff(uint32_t off) const { return (uint32_t)((uintptr_t)&_FS_start - XIP_BASE) + off; }

  void program256(uint32_t off, const uint8_t *page) override {
    noInterrupts();
    rp2040.idleOtherCore();
    flash_range_program(flashOff(off), page, 256);
    rp2040.resumeOtherCore();
    interrupts();
  }
  void erase4k(uint32_t off) override {
    noInterrupts();
    rp2040.idleOtherCore();
    flash_range_erase(flashOff(off), 4096);
    rp2040.resumeOtherCore();
    interrupts();
  }
};
PicoFlash g_flash;
}  // namespace

kvm::FlashIf &flashRegion() { return g_flash; }
