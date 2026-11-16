// kvm_hot.h — KVM_HOT(fn): place a hot-path function in RAM on the Pico
// (no flash/XIP cache-miss stalls), plain function on the PC test build.
#pragma once
#if defined(ARDUINO_ARCH_RP2040) || defined(PICO_BOARD)
#include <pico.h>
#define KVM_HOT(fn) __not_in_flash_func(fn)
#else
#define KVM_HOT(fn) fn
#endif
