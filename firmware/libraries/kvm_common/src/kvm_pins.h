// kvm_pins.h — THE wiring spec for the KVM. Single source of truth.
//
// Assemble the hardware from this file. Every signal lists:
//   GPnn (header pin NN)  ->  part : part-pin   [electrical notes]
// Change a pin here and the firmware follows; static_asserts at the bottom
// reject illegal choices at compile time.
//
// Board: Raspberry Pi Pico 2 (RP2350A). Header pin numbers count from pin 1
// (top-left, USB connector up, GP0) down the left side to pin 20, then pin 21
// (bottom-right, GP16) up the right side to pin 40 (VBUS).
//
// ============================================================================
//  POWER / GROUND DOMAINS  (read this first)
// ============================================================================
//
//  Domain 1 — "rail" (gaming-PC side)
//    5V external supply (DC jack screw terminal) feeds the breadboard rail.
//    On this rail: Pico A, USB hub, both relay modules, both solenoids,
//    both DP switches (via their USB-C power pigtails), ADuM1201 side 1.
//      Pico A pin 39 (VSYS) <- rail +5V
//      Pico A pin 38 (GND)  <- rail GND
//    Pico A's USB-C to the gaming PC carries data; the on-board diode stops
//    the rail back-feeding the PC.
//
//  Domain 2 — "work PC" (isolated)
//    Pico B is powered ONLY by its USB-C cable from the work PC.
//    Pico B VSYS / GND must NOT touch the rail. Its only link to domain 1 is
//    through the ADuM1201 isolator. Sharing a ground here would tie the two
//    PCs' grounds together and defeat the isolator.
//
//  ADuM1201 (8-pin, side 1 = pins 1-4, side 2 = pins 5-8)
//    pin 1 VDD1 <- Pico A pin 36 (3V3 OUT)     pin 8 VDD2 <- Pico B pin 36 (3V3 OUT)
//    pin 2 VIA  <- Pico A UART TX              pin 7 VOA  -> Pico B UART RX
//    pin 3 VOB  -> Pico A UART RX              pin 6 VIB  <- Pico B UART TX
//    pin 4 GND1 <- Pico A GND                  pin 5 GND2 <- Pico B GND
//    (If yours is a breakout board, match its silkscreen labels to these
//     names; the chip pin numbers above are for the bare SOIC-8.)
//
// ============================================================================
//  PICO A  (gaming PC side: USB host for keyboard/mouse + device to gaming PC)
// ============================================================================
//
//   Signal          GPIO   Header  Goes to
//   --------------  -----  ------  -------------------------------------------
//   LINK_UART TX    GP4    pin 6   ADuM1201 pin 2 (VIA)
//   LINK_UART RX    GP5    pin 7   ADuM1201 pin 3 (VOB)
//   RELAY_A IN      GP10   pin 14  relay module "A" IN  (drives monitor-1 DP switch)
//   RELAY_B IN      GP11   pin 15  relay module "B" IN  (drives monitor-2 DP switch)
//   SWITCH_BUTTON   GP15   pin 20  lid tact button, other leg -> GND
//   USB_HOST D+     GP16   pin 21  USB-A pigtail GREEN  (to hub upstream)
//   USB_HOST D-     GP17   pin 22  USB-A pigtail WHITE  (to hub upstream)
//   (USB-A pigtail RED -> rail +5V, BLACK -> rail GND. Not to the Pico.)
//   VSYS            —      pin 39  rail +5V
//   GND             —      pin 38  rail GND (any Pico GND pin works)
//   3V3 OUT         —      pin 36  ADuM1201 pin 1 (VDD1)
//
//   Relay module (each):  VCC -> rail +5V,  GND -> rail GND,  IN -> GPIO above.
//   Relay contacts (each): COM -> rail +5V,  NO -> solenoid lead 1,
//                          solenoid lead 2 -> rail GND.
//   Put a flyback diode (1N4007 / 1N5819) ACROSS each solenoid: stripe/cathode
//   to the +5V side lead. It twists into the same screw terminals. Without it
//   the solenoid's turn-off spike arcs the relay contacts and puts noise on
//   the rail that Pico A and the hub share.
//
//   DP wiring rule: the GAMING PC goes to DP input 1 on BOTH switches.
//
// ============================================================================
//  PICO B  (work PC side: device only)
// ============================================================================
//
//   Signal          GPIO   Header  Goes to
//   --------------  -----  ------  -------------------------------------------
//   LINK_UART TX    GP4    pin 6   ADuM1201 pin 6 (VIB)
//   LINK_UART RX    GP5    pin 7   ADuM1201 pin 7 (VOA)
//   GND             —      pin 38  ADuM1201 pin 5 (GND2)   -- NOT the rail
//   3V3 OUT         —      pin 36  ADuM1201 pin 8 (VDD2)
//   Power: USB-C from work PC only. Leave VSYS (pin 39) unconnected.
//
// ============================================================================
//  RESERVED on Pico 2 — do not assign
//    GP23 (SMPS mode), GP24 (VBUS sense), GP25 (on-board LED), GP29 (VSYS/3 ADC)
//    GP26-28 are free but are the only ADC pins; keep them for future sensing
//    (e.g. DP switch LED sensors).
// ============================================================================

#pragma once
#include <stdint.h>

namespace kvm {

// Header pin lookup for comments/debug output: header_pin_of(gp) -> 1..40, 0 = not on header.
constexpr uint8_t header_pin_of(int gp) {
  constexpr uint8_t map[29] = {
    1, 2, 4, 5, 6, 7, 9, 10, 11, 12,      // GP0-9
    14, 15, 16, 17, 19, 20, 21, 22,       // GP10-17
    24, 25, 26, 27, 29,                   // GP18-22
    0, 0, 0,                              // GP23-25 internal
    31, 32, 34                            // GP26-28
  };
  return (gp >= 0 && gp <= 28) ? map[gp] : 0;
}

constexpr int ONBOARD_LED = 25;   // GP25, on-board LED (both Picos)

// ---------------------------------------------------------------------------
namespace pico_a {
  // UART link to Pico B through the ADuM1201. Instance: 0 = uart0, 1 = uart1.
  constexpr int LINK_UART    = 1;
  constexpr int LINK_TX      = 4;    // header pin 6  -> ADuM1201 VIA (pin 2)
  constexpr int LINK_RX      = 5;    // header pin 7  <- ADuM1201 VOB (pin 3)

  // Relay inputs. Active level / drive mode is set in kvm_config.h
  // (RELAY_ACTIVE_LOW, RELAY_OPEN_DRAIN); verify with the relay-probe sketch.
  constexpr int RELAY_A      = 10;   // header pin 14 -> relay A IN (monitor 1)
  constexpr int RELAY_B      = 11;   // header pin 15 -> relay B IN (monitor 2)

  // Lid button: internal pull-up, pressed = LOW. Other leg to GND.
  constexpr int SWITCH_BTN   = 15;   // header pin 20

  // PIO-USB host. D- MUST be D+ + 1 (the PIO program uses consecutive pins).
  // Our boards are RP2350 stepping A4 (marking RP2350A0A4), which fixes
  // erratum E9, so the internal pull-downs work and no external resistors
  // are needed. On an older A2/A3 board, if devices aren't detected, add
  // a 15 kOhm resistor from each of D+ and D- to GND.
  constexpr int USB_HOST_DP  = 16;   // header pin 21 <- pigtail GREEN
  constexpr int USB_HOST_DM  = 17;   // header pin 22 <- pigtail WHITE
}

// ---------------------------------------------------------------------------
namespace pico_b {
  constexpr int LINK_UART    = 1;
  constexpr int LINK_TX      = 4;    // header pin 6  -> ADuM1201 VIB (pin 6)
  constexpr int LINK_RX      = 5;    // header pin 7  <- ADuM1201 VOA (pin 7)
}

// ===========================================================================
//  Compile-time checks. If one fires, the message says what is wrong.
// ===========================================================================
namespace detail {
  constexpr bool on_header(int gp) { return header_pin_of(gp) != 0; }

  constexpr bool reserved(int gp) { return gp == 23 || gp == 24 || gp == 25 || gp == 29; }

  // RP2350 UART function (F2) pin options available on the Pico 2 header.
  constexpr bool uart_tx_ok(int uart, int gp) {
    return uart == 0 ? (gp == 0 || gp == 12 || gp == 16 || gp == 28)
         : uart == 1 ? (gp == 4 || gp == 8  || gp == 20)
         : false;
  }
  constexpr bool uart_rx_ok(int uart, int gp) {
    return uart == 0 ? (gp == 1 || gp == 13 || gp == 17)
         : uart == 1 ? (gp == 5 || gp == 9  || gp == 21)
         : false;
  }

  template <int N>
  constexpr bool all_distinct(const int (&a)[N]) {
    for (int i = 0; i < N; ++i)
      for (int j = i + 1; j < N; ++j)
        if (a[i] == a[j]) return false;
    return true;
  }

  template <int N>
  constexpr bool all_usable(const int (&a)[N]) {
    for (int i = 0; i < N; ++i)
      if (!on_header(a[i]) || reserved(a[i])) return false;
    return true;
  }

  constexpr int PICO_A_PINS[] = {
    pico_a::LINK_TX, pico_a::LINK_RX, pico_a::RELAY_A, pico_a::RELAY_B,
    pico_a::SWITCH_BTN, pico_a::USB_HOST_DP, pico_a::USB_HOST_DM,
  };
  constexpr int PICO_B_PINS[] = { pico_b::LINK_TX, pico_b::LINK_RX };
}

static_assert(detail::all_distinct(detail::PICO_A_PINS), "Pico A: a GPIO is assigned twice");
static_assert(detail::all_distinct(detail::PICO_B_PINS), "Pico B: a GPIO is assigned twice");
static_assert(detail::all_usable(detail::PICO_A_PINS),   "Pico A: pin is reserved or not on the header");
static_assert(detail::all_usable(detail::PICO_B_PINS),   "Pico B: pin is reserved or not on the header");
static_assert(pico_a::USB_HOST_DM == pico_a::USB_HOST_DP + 1, "PIO-USB needs D- = D+ + 1");
static_assert(detail::uart_tx_ok(pico_a::LINK_UART, pico_a::LINK_TX), "Pico A: LINK_TX not a TX pin for that UART");
static_assert(detail::uart_rx_ok(pico_a::LINK_UART, pico_a::LINK_RX), "Pico A: LINK_RX not an RX pin for that UART");
static_assert(detail::uart_tx_ok(pico_b::LINK_UART, pico_b::LINK_TX), "Pico B: LINK_TX not a TX pin for that UART");
static_assert(detail::uart_rx_ok(pico_b::LINK_UART, pico_b::LINK_RX), "Pico B: LINK_RX not an RX pin for that UART");

}  // namespace kvm
