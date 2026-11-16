// hid_reports.h — the fixed HID report formats both Picos present to their PC.
// Pure data, no TinyUSB dependency, so host-side tests can include it.
//
// USB layout (identical on Pico A and Pico B):
//   HID interface 0: keyboard, boot-protocol compatible (works in BIOS / BitLocker),
//                    no report ID, 8-byte report, LED output via SET_REPORT.
//   HID interface 1: mouse (report ID 1) + consumer/media keys (report ID 2).
// Each interface has its own interrupt IN endpoint polled every 1 ms, so keyboard
// traffic never delays a mouse report. Consumer shares the mouse endpoint because
// the TinyUSB build allows only 2 HID interfaces; media keys are rare enough that
// this costs nothing in practice.
#pragma once
#include <stdint.h>

namespace kvm {

constexpr uint8_t RID_MOUSE    = 1;
constexpr uint8_t RID_CONSUMER = 2;

// Standard boot keyboard report.
struct __attribute__((packed)) KbdReport {
  uint8_t modifiers;   // bit0 LCtrl, 1 LShift, 2 LAlt, 3 LGui, 4 RCtrl, 5 RShift, 6 RAlt, 7 RGui
  uint8_t reserved;
  uint8_t keys[6];     // HID usage IDs (page 0x07), 0 = none
};

// Mouse: 16-bit deltas so high-DPI mice pass through without clipping.
struct __attribute__((packed)) MouseReport {
  uint8_t buttons;     // bit0 left, 1 right, 2 middle, 3 back, 4 forward
  int16_t x;
  int16_t y;
  int8_t  wheel;       // vertical
  int8_t  pan;         // horizontal (AC Pan)
};

static_assert(sizeof(KbdReport) == 8, "KbdReport must be 8 bytes (boot protocol)");
static_assert(sizeof(MouseReport) == 7, "MouseReport layout changed; update the descriptor");

// Keyboard LED output bits (from the PC).
namespace led {
  constexpr uint8_t NUM    = 0x01;
  constexpr uint8_t CAPS   = 0x02;
  constexpr uint8_t SCROLL = 0x04;
}

// Mouse report descriptor matching MouseReport, with report ID 1.
// Kept as a macro so it can be concatenated with the consumer descriptor.
#define KVM_HID_DESC_MOUSE16                                                   \
  0x05, 0x01,        /* Usage Page (Generic Desktop)            */            \
  0x09, 0x02,        /* Usage (Mouse)                           */            \
  0xA1, 0x01,        /* Collection (Application)                */            \
  0x85, 0x01,        /*   Report ID (1)                         */            \
  0x09, 0x01,        /*   Usage (Pointer)                       */            \
  0xA1, 0x00,        /*   Collection (Physical)                 */            \
  0x05, 0x09,        /*     Usage Page (Button)                 */            \
  0x19, 0x01,        /*     Usage Minimum (1)                   */            \
  0x29, 0x05,        /*     Usage Maximum (5)                   */            \
  0x15, 0x00,        /*     Logical Minimum (0)                 */            \
  0x25, 0x01,        /*     Logical Maximum (1)                 */            \
  0x95, 0x05,        /*     Report Count (5)                    */            \
  0x75, 0x01,        /*     Report Size (1)                     */            \
  0x81, 0x02,        /*     Input (Data,Var,Abs)                */            \
  0x95, 0x01,        /*     Report Count (1)                    */            \
  0x75, 0x03,        /*     Report Size (3)                     */            \
  0x81, 0x01,        /*     Input (Const) padding               */            \
  0x05, 0x01,        /*     Usage Page (Generic Desktop)        */            \
  0x09, 0x30,        /*     Usage (X)                           */            \
  0x09, 0x31,        /*     Usage (Y)                           */            \
  0x16, 0x01, 0x80,  /*     Logical Minimum (-32767)            */            \
  0x26, 0xFF, 0x7F,  /*     Logical Maximum (32767)             */            \
  0x75, 0x10,        /*     Report Size (16)                    */            \
  0x95, 0x02,        /*     Report Count (2)                    */            \
  0x81, 0x06,        /*     Input (Data,Var,Rel)                */            \
  0x09, 0x38,        /*     Usage (Wheel)                       */            \
  0x15, 0x81,        /*     Logical Minimum (-127)              */            \
  0x25, 0x7F,        /*     Logical Maximum (127)               */            \
  0x75, 0x08,        /*     Report Size (8)                     */            \
  0x95, 0x01,        /*     Report Count (1)                    */            \
  0x81, 0x06,        /*     Input (Data,Var,Rel)                */            \
  0x05, 0x0C,        /*     Usage Page (Consumer)               */            \
  0x0A, 0x38, 0x02,  /*     Usage (AC Pan)                      */            \
  0x15, 0x81,        /*     Logical Minimum (-127)              */            \
  0x25, 0x7F,        /*     Logical Maximum (127)               */            \
  0x75, 0x08,        /*     Report Size (8)                     */            \
  0x95, 0x01,        /*     Report Count (1)                    */            \
  0x81, 0x06,        /*     Input (Data,Var,Rel)                */            \
  0xC0,              /*   End Collection                        */            \
  0xC0               /* End Collection                          */

}  // namespace kvm
