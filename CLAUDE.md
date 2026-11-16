# DIY KVM with solenoid-switched DP switches (firmware)

Hobby embedded project. I build the hardware and write the firmware myself as a learning project, so don't suggest off-the-shelf KVM firmware (DeskHop etc.). Hardware is solder-free: breadboard, dupont wires, screw terminals. All parts are in hand; see `build_guide.html` (KVM section) for the parts and enclosure. The Stream Deck is a separate project developed in parallel; it does not interact with the KVM.

Top priorities: **smooth transitions** and **minimal added input latency**.

## Hardware
- **Pico A** (Pico 2 / RP2350):
  - PIO-USB host → USB 2.0 hub → keyboard, mouse cable, mouse dongle.
  - Native USB device → **gaming PC**.
  - Powered from the 5V rail via VSYS.
- **Pico B** (Pico 2): native USB device → **work PC**. Powered only by the work PC's USB, never from the rail.
- **Link:** UART A↔B through an **ADuM1201** isolator. Side 1 is powered by Pico A, side 2 by Pico B.
- **Monitor switching:** 2× 2:1 DP 1.4 switches, one per monitor. Their SELECT buttons are pressed by 5V solenoids through 5V relay modules. The DP switches toggle on each press. The gaming PC goes to DP input 1 on both.
- **Trigger:** one tact button on the lid.
- **Power:** a 5V/2A external rail feeds Pico A, the hub, relays, solenoids and the DP switches.
- **No work PC yet.** For testing, plug Pico B into a second USB port on the gaming PC.
- **Board IDs** (unique ID from `01_chipinfo`):

  | Board | Unique ID | Notes |
  |---|---|---|
  | Pico A | `5B3009CC81C08F23` | Same clone type as B (Boya flash, rev 3, bogus VSYS reading). Chip marking `RP2350A0A4` (stepping A4, erratum E9 fixed). The A4 chip reports CHIP_ID revision 3. |
  | Pico B | `80A2839148DA51D9` | Clone Pico 2: Boya 4 MB flash, chip marking `RP2350A0A4` (stepping A4, reports CHIP_ID revision 3). The VSYS ADC reading is bogus (2.9 V on USB), so don't rely on GP29/GP24. |

## Pins and config
- **All pin assignments live in `firmware/libraries/kvm_common/src/kvm_pins.h`.** It is the wiring spec: every pin comments its header pin number and what it connects to, and `static_assert`s reject illegal choices. Never hard-code a GPIO anywhere else.
- Tunables (baud, relay polarity, pulse/lockout times, DP power-on mode, feature flags) are in `kvm_config.h`. They can be overridden with `-D`.

## Architecture (decided)
- **Pico A core 1:** PIO-USB host → own HID report-descriptor parser → normalized events → router. Core 1 is the sole UART owner.
  - Target B: send over UART directly.
  - Target A: SPSC ring to core 0.
  - USB host = Adafruit TinyUSB host stack on PIO-USB (`Adafruit_USBH_Host`, `configure_pio_usb`), run from `setup1()/loop1()`. TinyUSB defaults HID devices to **boot protocol**; call `tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT)` before `begin`. Report descriptors > 256 bytes arrive as NULL in `tuh_hid_mount_cb` and must be fetched with `tuh_descriptor_get_hid_report`.
- **Pico A core 0:** TinyUSB device, switch controller, relay state machine, press log, CDC console.
  - The device stack's `tud_task` runs in a soft IRQ on core 0 holding `__usb_mutex`; main-loop code that calls `tud_hid_*` must hold that mutex too.
- **Logging:** never `Serial.print` from core 1 or hot code; use `kvm::log::printf` (`kvm_log.h`, RAM ring) and drain it from the core 0 loop.
- **Pico B:** UART RX (COBS + CRC8) → TinyUSB device.
- **Both Picos expose identical fixed descriptors** (`hid_reports.h`, `usb_device.*`), and both stay enumerated permanently, with no USB detach on a switch.
  - HID interface 0: boot keyboard.
  - HID interface 1: mouse (16-bit XY, report ID 1) + consumer (report ID 2).
  - Each interface has its own 1 ms endpoint. Consumer shares the mouse endpoint because the TinyUSB build caps it at `CFG_TUD_HID=2`, and the IDE can't override that.
  - The "PC collected a report" hook is our own `tud_hid_report_complete_cb` in `usb_device.cpp`. Don't define it elsewhere.
- **Latency rules:**
  - Coalesce mouse motion; queue only button and key edges.
  - Send from `tud_hid_report_complete_cb`.
  - Hot path in RAM (`__not_in_flash_func`); nothing blocking on the hot path.
  - Flash writes happen only at switch time (a single page program) or while input is idle.
- **Switching:**
  - Act on the button press edge; input moves immediately.
  - Release everything on the old PC; suppress keys still held on the new PC until they're released.
  - Lock LEDs follow the active PC.
  - Both relays fire together via `gpio_put_masked`, followed by a lockout window; extra presses collapse to one pending toggle.
- **DP state:** open loop, plus a press log in a raw flash ring (the 64 KB FS region; LittleFS is not used). Console commands: `state`, `nudge a|b`, `baseline`. `KVM_DP_POWERON_MODE` sets whether the switches reset to input 1 at power-up or remember the last input.
- **USB is full speed:** 1 kHz is the polling ceiling. Planned improvement: SOF phase lock (align host polling to the PC's frame timing).

Full design rationale (latency budget, power-loss handling, test bench, hardware identification): `docs/architecture.md`.

## Toolchain
**Primary environment: Arduino IDE 2.** arduino-cli (`scripts/build.ps1`) is for scripted or headless builds; both use the same installed cores and libraries.

Arduino IDE setup:
- **`kvm_common` reaches the IDE through a directory junction:** `Documents\Arduino\libraries\kvm_common` points to `firmware\libraries\kvm_common` in this repo. Edits in the repo show up in the IDE immediately. To recreate it:
  `New-Item -ItemType Junction -Path "$env:USERPROFILE\Documents\Arduino\libraries\kvm_common" -Target "<repo>\firmware\libraries\kvm_common"`
- **Tools menu** (must match the FQBN below):

  | Setting | Value |
  |---|---|
  | Board | Raspberry Pi Pico 2 |
  | USB Stack | Adafruit TinyUSB |
  | CPU Speed | 120 MHz |
  | Optimize | -O2 |
  | Flash Size | 4MB (Sketch: 4032KB, FS: 64KB) |
  | CPU Architecture | ARM (not RISC-V) |
- Tunables: the IDE can't pass `-D`, so edit the defaults in `kvm_config.h` directly.
- For board updates, add the arduino-pico URL in IDE Preferences → Additional boards manager URLs: `https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json`.
- **TinyUSB library conflict:** the user-installed `Documents\Arduino\libraries\Adafruit_TinyUSB_Library` overrides the copy bundled in arduino-pico. Both are 3.7.7 now. Keep them on the same version: don't accept a library-manager update for it unless arduino-pico updates too. If the KVM build breaks after any update, check this first.

Core and libraries:
- arduino-pico (Earle Philhower) **6.3.x**. arduino-cli installed it via `--additional-urls`.
- Libraries:
  - Adafruit TinyUSB 3.7.7 (bundled with the core and also installed as a user library).
  - **Pico PIO USB 0.7.3**: user library only, required for Pico A.
- arduino-cli is not on PATH. `scripts/build.ps1` falls back to the copy bundled with Arduino IDE 2.
- FQBN: `rp2040:rp2040:rpipico2:usbstack=tinyusb,freq=120,opt=Optimize2,flash=4194304_65536`. PIO-USB needs the clock to be a multiple of 12 MHz: 120, or 240 MHz to try.
- Every sketch must `#include <Adafruit_TinyUSB.h>`. Without it, `Serial` fails to link under `usbstack=tinyusb`.
- `kvm_common` uses `dot_a_linkage=true`: library objects link only when a sketch references them. That lets a bring-up sketch define its own `tuh_*` callbacks as long as it doesn't use `usb_host.*`. Strong callbacks in the library override TinyUSB's weak defaults once pulled in (check with `arm-none-eabi-nm` if in doubt).
- Host-side unit tests (pure C++ parts: parser, model, queue, link protocol, press log, PLL) run in WSL g++ with ASan/UBSan: `.\scripts\test.ps1`. Keep those modules free of Arduino/pico includes (use `kvm_hot.h` for RAM placement).
- Python on this machine is the Windows Store stub (it hangs waiting for input). Don't call `python`/`python3` from Bash; use WSL's `python3` if needed.

```powershell
.\scripts\build.ps1 firmware\bringup\01_chipinfo            # compile
.\scripts\build.ps1 firmware\bringup\01_chipinfo -Port COM7 # compile + upload
.\scripts\build.ps1 -ListPorts
.\scripts\test.ps1                                          # host unit tests (WSL)
```
First flash of a blank Pico: hold BOOTSEL while plugging it in.

## Layout
```
firmware/libraries/kvm_common/   shared library (see src/*.h headers for each module's role)
firmware/bringup/NN_name/        one small test sketch per step
firmware/pico_a/, pico_b/        main firmware
firmware/tests/                  host-side unit tests (scripts/test.ps1)
scripts/build.ps1, test.ps1      arduino-cli wrapper, host tests
```

## Build order
1. Toolchain and chip-info on both Picos (`01_chipinfo`).
2. Pico B: fixed HID descriptors with a synthetic mouse jiggle and keypress.
3. Pico A: PIO-USB host through the hub; dump descriptors and reports.
4. HID parser and normalization, with host-side tests on the captured descriptors.
5. Pico A passthrough to the gaming PC, with a latency histogram.
6. UART link through the ADuM1201: baud-sweep probe, protocol, heartbeat.
7. Switch controller: release-all, held-key suppression, LED sync, remote wakeup.
8. Relays and solenoids:
   - relay-probe sketch, masked pulse and lockout
   - press log and console
   - test DP power-on behavior
9. Soak test.
10. SOF phase lock.

## Working style
- Build incrementally, one subsystem per step, each with a small test sketch I can flash and check.
- Explain embedded-specific choices briefly. I'm a software engineer with ESP32/Arduino experience.
- Keep build and flash commands runnable with arduino-cli.
