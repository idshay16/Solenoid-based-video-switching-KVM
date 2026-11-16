// kvm_config.h — tunables. Pins live in kvm_pins.h, not here.
// Every value can be overridden with -D on the compile line (see scripts/build.ps1).
#pragma once
#include <stdint.h>

// ---------------------------------------------------------------- link (UART)
// ADuM1201 grade sets the ceiling: A = 1 Mbps, B = 10 Mbps, C = 25 Mbps.
// Start at 1 Mbaud; the link-probe sketch (step 6) measures what actually works.
#ifndef KVM_LINK_BAUD
#define KVM_LINK_BAUD             1000000u
#endif
#ifndef KVM_HEARTBEAT_MS
#define KVM_HEARTBEAT_MS          50u      // A -> B heartbeat period
#endif
#ifndef KVM_HEARTBEAT_MISSES
#define KVM_HEARTBEAT_MISSES      3u       // B releases all after this many misses
#endif

// ---------------------------------------------------------------- relays / solenoids
// Relay-probe sketch (step 8) tells you which of these your modules need.
#ifndef KVM_RELAY_ACTIVE_LOW
#define KVM_RELAY_ACTIVE_LOW      1        // 1: IN pulled LOW energises relay (typical opto module)
#endif
#ifndef KVM_RELAY_OPEN_DRAIN
#define KVM_RELAY_OPEN_DRAIN      0        // 1: "off" = pin floats (input) instead of driving HIGH.
#endif                                     //    Use if a 3.3V HIGH doesn't fully release a 5V module.
#ifndef KVM_DP_PULSE_MS
#define KVM_DP_PULSE_MS           100u     // solenoid on-time per press
#endif
#ifndef KVM_DP_LOCKOUT_MS
#define KVM_DP_LOCKOUT_MS         800u     // ignore/collapse new switch requests for this long
#endif
#ifndef KVM_DP_FIRE_STAGGER_MS
#define KVM_DP_FIRE_STAGGER_MS    0u       // >0 fires relay B this much after A (only if inrush resets Pico A)
#endif

// What the DP switches do at power-up (measure in step 8: power-cycle 5x, note the port).
#define KVM_DP_POWERON_PORT1      0        // always come up on input 1 -> state resets at boot
#define KVM_DP_POWERON_REMEMBER   1        // remember last input       -> restore from press log
#ifndef KVM_DP_POWERON_MODE
#define KVM_DP_POWERON_MODE       KVM_DP_POWERON_PORT1
#endif

// ---------------------------------------------------------------- switching behaviour
#ifndef KVM_BUTTON_DEBOUNCE_MS
#define KVM_BUTTON_DEBOUNCE_MS    5u       // acts on press edge; this is the re-arm time
#endif
#ifndef KVM_INPUT_SWITCH_DELAY_MS
#define KVM_INPUT_SWITCH_DELAY_MS 0u       // 0 = keyboard/mouse move the instant solenoids fire
#endif
#ifndef KVM_HOTKEY_ENABLED
#define KVM_HOTKEY_ENABLED        0        // optional keyboard hotkey; box button is the primary trigger
#endif
#ifndef KVM_HOTKEY_MODS
#define KVM_HOTKEY_MODS           0x00     // modifier bits that must be held (0x01 LCtrl 0x02 LShift 0x04 LAlt 0x10 RCtrl ...)
#endif
#ifndef KVM_HOTKEY_KEY
#define KVM_HOTKEY_KEY            0x47     // HID usage of the trigger key (0x47 = Scroll Lock); never reaches either PC
#endif
#ifndef KVM_BUTTON_CONFIRM_US
#define KVM_BUTTON_CONFIRM_US     1000u    // button must read pressed this long (noise filter) before acting
#endif

// ---------------------------------------------------------------- latency
#ifndef KVM_SOF_PHASE_LOCK
#define KVM_SOF_PHASE_LOCK        0        // step 10: align host polling to the PC's SOF
#endif

// ---------------------------------------------------------------- debug
#ifndef KVM_DEBUG_CDC
#define KVM_DEBUG_CDC             1        // expose a CDC serial port for logs + console
#endif
