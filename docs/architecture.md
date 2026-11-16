# KVM firmware architecture

## Context
This is a DIY KVM, and all parts are in hand (see `build_guide.html` (local, not in git), KVM section). The hardware: 2× Pico 2 (RP2350); a USB 2.0 hub carrying keyboard, mouse cable and mouse dongle; an ADuM1201 isolator; 2× 5V relay modules → 2× mini push-pull solenoids pressing the SELECT button on 2× 2:1 DP 1.4 switches (one per monitor); one tact button on the lid as the trigger; and a 5V/2A external rail.
- Pico A is the USB device to the **gaming PC** and the PIO-USB host for the hub.
- Pico B is the USB device to the **work PC**. There is no work PC yet; see the test bench below.

The repo is empty, and this repo is KVM only. The Stream Deck is developed in parallel in its own project and doesn't interact with the KVM.

**Goals:** smooth transitions and minimal added input latency. Solder-free breadboard. arduino-pico + Pico-PIO-USB + Adafruit TinyUSB, built with arduino-cli. Each step is a small flashable test sketch.

## Decisions locked
- **Trigger:** the box button, acting on the **press edge**. An optional keyboard hotkey sits behind a config flag, off by default.
- **Input follows immediately:** input moves when the solenoids fire. `INPUT_SWITCH_DELAY_MS` is a config constant, default 0.
- **DP state:** open loop + a persistent **press log** in flash. State per switch = (baseline + press count) mod 2. Serial console commands: `state`, `nudge a|b`, `baseline <a> <b>`.
- **Pins:** everything lives in one header, with wiring comments clear enough to assemble the hardware from the code.
- **Isolation fix (approved):** Pico B is powered from the work PC's USB only. It doesn't touch the 5V rail or its GND. The build guide gets updated to match.

## Topology
```
KB / mouse / dongle ─ USB hub (5V rail) ─ D+/D- ─┐
                                          [Pico A] ── USB-C ── Gaming PC     (VSYS from 5V rail)
 lid button ── GPIO ─────────────────────────┤
 relay A/B ── GPIO ──→ solenoid A/B → DP switch SELECT
                                              │ UART            ← rail / gaming-PC ground domain
                                          ADuM1201  ═══ isolation barrier ═══
                                              │ UART            ← work-PC ground domain
                                          [Pico B] ── USB-C ── Work PC       (powered by work PC VBUS only)
```

## Latency and polling rate
**Hard cap: 1 kHz.** The RP2350 USB port and PIO-USB are both full-speed only (12 Mbit/s). Full-speed interrupt endpoints are polled at most once per 1 ms frame, so 4k/8k mice run at 1 kHz through this KVM. Going beyond that needs high-speed USB (480 Mbit/s) on both sides, e.g. a Teensy 4.1. That's a different board and isn't planned.

**What we can still improve: phase alignment ("SOF phase lock").** Most of the added latency is two unsynchronized 1 ms clocks. Our host polls the mouse, and later the PC polls us. The wait between them is a random 0–1 ms (avg 0.5 ms). Pico A sees the active PC's start-of-frame (`tud_sof_cb`; Pico B forwards its PC's SOF timing over UART when B is active). Firmware then shifts the PIO-USB host's 1 ms frame timer so the mouse is polled ~100–150 µs **before** the PC polls us, and keeps trimming the timer to follow clock drift.

| | Baseline | With SOF phase lock |
|---|---|---|
| Added latency, avg | ~0.5 ms | ~0.15 ms |
| Added latency, worst | ~1.2 ms | ~0.3 ms |
This is built as a separate step after everything works (step 10), with a config flag and before/after measurements.

Hot-path rules:
- **Coalesce** mouse motion. **Queue only button/key edges**, so fast clicks are never lost and there's no backlog.
- **One HID interface per report type** (kbd / mouse / consumer), each with its own IN endpoint, so a keyboard report never delays a mouse report.
- Send the next report from `tud_hid_report_complete_cb`.
- Hot code runs from RAM (`__not_in_flash_func`).
- Logs go to a RAM ring, drained when idle.
- Flash rule: see the power-loss section. A ~1 ms page write happens only at switch time, and erases only when input has been idle.

## Smooth-transition rules
1. **Both Picos stay enumerated at all times.** No USB detach on a switch.
2. On a switch: send all-released to the old PC and drop any pending motion.
3. Keys held during a switch are suppressed on the new PC until released.
4. Lock LEDs follow the active PC: B forwards the work PC's LED report, and A sends SET_REPORT to the keyboard on a switch.
5. If the target PC is suspended, the first input calls `tud_remote_wakeup()`.
6. Both relays fire in one `gpio_put_masked`. `DP_PULSE_MS` (~100) on, then `DP_LOCKOUT_MS` (~800). Presses during lockout collapse to at most one pending toggle.
7. Link health: A sends a heartbeat every 50 ms and B acks it. If B misses 3, it releases all. If A gets no acks, it marks B offline (video still switches).
8. Relay pins are driven inactive as the very first line of `setup()`.

## Power loss and resets
| Event | What happens | Handling |
|---|---|---|
| **5V rail lost** (whole box) | Pico A, hub, relays and both DP switches die together. Pico B stays alive on work-PC power. | B sees the heartbeat stop → releases all keys/buttons on the work PC (nothing stuck). On power return, see "boot" below. |
| **Rail returns / Pico A boots** | DP switches power up in their default state. | Depends on DP switch behavior, measured in step 8: **(a) the switch always powers up on port 1** → reset the log baseline to port 1 at every boot, and the press log only matters within one power session; **(b) the switch remembers its last port** → restore state from the press log. Config `DP_POWERON_MODE = PORT1 \| REMEMBER`. **Wiring rule: the gaming PC goes to DP port 1 on both switches**, so (a) means boot = gaming PC on both monitors = Pico A active. Consistent with no special logic. |
| **Power cut mid-switch** | A press might land without being logged, or vice versa. | Press log = raw flash ring (not LittleFS): sectors are pre-erased while input is idle, and each press appends one 16-byte record right after the pulse (~1 ms page program, which pauses the USB host for about one frame at switch time, when it doesn't matter). Loss window ≈ the 100 ms pulse. In mode (a) this resolves itself anyway. `nudge` repairs any leftover drift. |
| **Gaming PC off / reboot** | Pico A keeps running (rail power) and keeps reading the keyboard/mouse. | No action. Re-enumerates when the PC comes back. |
| **Work PC off / reboot** | Pico B loses power or resets. | A marks the link offline. When B returns, A resends the active state and LED state, and B starts with everything released. |
| **Brownout during relay pulse** | Solenoid inrush sags the rail. | Rail budget is fine per the build guide (1.6 A peak vs 2 A supply). If Pico A resets during testing, add a staggered-fire option (`DP_FIRE_STAGGER_MS`, default 0). |

## Pin configuration: `firmware/libraries/kvm_common/src/kvm_pins.h`
This is the single source of truth for both boards (`namespace pico_a` / `namespace pico_b`). Each pin is a `constexpr` with a comment giving:
- the GPIO number + **physical header pin number**
- the target part and its pin name
- pull/active level and voltage domain

The top of the file has an ASCII wiring table per board, plus the power pins. `static_assert`s enforce:
- PIO-USB D− = D+ + 1
- UART pins are legal for the chosen UART instance
- no duplicate GPIOs
- reserved pins avoided (GP23–25, GP29)

`kvm_config.h` holds the tunables: baud, pulse/lockout/stagger ms, `RELAY_ACTIVE_LOW`, relay drive mode (push-pull / open-drain), `DP_POWERON_MODE`, hotkey enable, `INPUT_SWITCH_DELAY_MS`, `SOF_PHASE_LOCK`.

Proposed defaults:
| Signal | Pico A | Pico B | Wires to |
|---|---|---|---|
| UART1 TX | GP4 (pin 6) | GP4 (pin 6) | A→ADuM pin 2 VIA · B→ADuM pin 6 VIB |
| UART1 RX | GP5 (pin 7) | GP5 (pin 7) | A←ADuM pin 3 VOB · B←ADuM pin 7 VOA |
| Relay A IN | GP10 (pin 14) | — | relay module A `IN` |
| Relay B IN | GP11 (pin 15) | — | relay module B `IN` |
| Lid button | GP15 (pin 20) | — | button → GND, internal pull-up |
| USB host D+ / D− | GP16 / GP17 (pins 21/22) | — | USB-A pigtail green / white |
| Power | VSYS pin 39 ← rail 5V, GND pin 38 ← rail GND | **USB-C only; no rail connection** | — |
| ADuM side power | VDD1 (pin 1) ← A 3V3 (pin 36), GND1 (pin 4) ← A GND | VDD2 (pin 8) ← B 3V3 (pin 36), GND2 (pin 5) ← B GND | — |

## Identifying the hardware
**Read by eye:**
| Part | Where to look | What it tells us |
|---|---|---|
| ADuM1201 | 8-pin chip on the isolator breakout. Top marking, e.g. `ADuM1201ARZ` / `BRZ` / `CRZ` | The letter after `1201` sets max speed: A = 1 Mbps, B = 10, C = 25. A clone may show a different part number (e.g. `π121`, `CA-IS372x`); note it and we'll check its datasheet. |
| Relay module | Small 4-pin chip near the IN pin (`817` = PC817 optocoupler); transistor marking (`J3Y` = S8050); any H/L trigger jumper; relay can text (e.g. `SRD-05VDC-SL-C`) | Active-low vs active-high, and whether a 3.3V GPIO drives it reliably. |
| USB hub board | Largest chip on the hub (`FE1.1s`, `GL850G`, `SL2.1A`…) | Hub controller type (firmware can confirm this; see below). |
| Pico 2 | Main chip `RP2350A…`; small 8-pin flash chip (`W25Q32` = 4 MB, `W25Q128` = 16 MB) | Confirms a real RP2350 and the flash size used for the press log (firmware can confirm this too). |

**Pulled automatically by bring-up firmware**, printed over USB serial:
- **Step 1 (both Picos):** chip ID/revision, flash JEDEC ID → flash size, unique board ID, sysclk.
- **Step 3 (Pico A host):** for the hub and every keyboard/mouse/dongle: VID/PID, manufacturer/product strings, speed (LS/FS), endpoint polling interval (bInterval = native poll rate), the full HID report descriptors, and the hub descriptor (TT type). The VID identifies the hub chip (e.g. `1a40` = FE1.1s, `05e3` = Genesys).
- **Step 6 (link probe):** the ADuM1201 has no electronic ID, so we **measure it**: run PING/PONG bursts at 1, 2, 3, 4 and 6 Mbaud and count CRC errors. Firmware then uses the highest clean rate minus one step. This also catches wiring and noise problems.
- **Step 8 (relay probe):** toggle each relay pin through each drive mode (active-high, active-low push-pull, active-low open-drain), and you confirm click/LED. This settles `RELAY_ACTIVE_LOW` and the drive mode.

## Firmware architecture
**Pico A** runs sysclk at a multiple of 12 MHz: 120 MHz by default, 240 MHz to test.
- **Core 1:** PIO-USB host task. Report callback → `hid_parser` → normalized event → router. Core 1 is the sole UART owner:
  - active = B: TX directly
  - active = A: SPSC ring → core 0
  - it also handles RX from B and drains a small control queue from core 0
- **Core 0:** TinyUSB device task, switch controller, relay state machine, press log, CDC console, log drain, SOF phase lock (step 10). The active PC is one atomic byte.

**Pico B:** UART RX IRQ → COBS + CRC8 → report state → tight loop with `tud_task` + send. It sends LED state, heartbeat acks and SOF timing back.

**`kvm_common` library:**
- `hid_parser`: our own report-descriptor parser (field bit offsets for buttons, X/Y 8/12/16-bit, wheel, pan, kbd 6KRO/NKRO, consumer, report IDs), with boot protocol as the fallback
- `hid_norm`: normalized event structs
- `usb_desc`: identical fixed descriptors on both Picos, one interface per report type, CDC behind a flag
- `link_proto`: COBS + CRC8. Messages: KBD, MOUSE, CONSUMER, RELEASE_ALL, HEARTBEAT/ACK, LED_STATE, SOF_TIMING, PING/PONG

## Repo layout
```
CLAUDE.md                       KVM context, updated: parts in hand, these decisions
build_guide.html                (existing; updated for Pico B power/isolation)
firmware/
  libraries/kvm_common/src/     kvm_pins.h, kvm_config.h, hid_parser, hid_norm, usb_desc, link_proto, cobs, crc8
  pico_a/                       pico_a.ino, host.cpp, router.cpp, switcher.cpp, relay.cpp, presslog.cpp, console.cpp, sofsync.cpp
  pico_b/                       pico_b.ino, link_rx.cpp, device.cpp
  tests/                        host-side g++ tests: parser on captured descriptors, COBS/CRC, press-log replay
  bringup/                      one small sketch per step below
scripts/build.ps1               arduino-cli compile/upload wrappers
```
Build: `arduino-cli compile --fqbn rp2040:rp2040:rpipico2:usbstack=tinyusb,freq=120 --libraries firmware/libraries firmware/pico_a`. Confirm the menu keys with `arduino-cli board details` in step 1.

## Build-guide update (approved isolation fix)
Edit the KVM notes in `build_guide.html`:
- Replace "Both Picos powered from the 5V rail via VSYS" with: Pico A on the rail via VSYS; **Pico B powered only from the work PC's USB-C, with no wire to the rail 5V or GND**.
- Add: ADuM1201 side 1 (VDD1/GND1) goes to Pico A's 3V3/GND; side 2 (VDD2/GND2) goes to Pico B's 3V3/GND. The two sides must never share a ground.
- Fix the "keep all grounds common" line so it excludes Pico B.
- Add the wiring rule: the gaming PC goes to DP port 1 on both switches.

## Test bench (no work PC yet)
- **Pico B → a second USB port on the gaming PC.** Windows sees two separate keyboard/mouse sets. Switching moves input from "Pico A's" devices to "Pico B's" devices, and both type and move the same cursor, so everything except true two-PC video can be verified. Same PC means shared ground, so isolation isn't tested here, which is fine on the bench.
- Optional second target: any laptop, or an Android phone with USB-OTG (it accepts USB keyboard/mouse).
- A host-side tool (`scripts/hidmon.py`, using `hid`/`hidapi`) shows which Pico a report came from, with timestamps, to verify routing and release-all.
- DP switches + monitors can be tested with the gaming PC feeding both DP inputs, or just observing the port LEDs.

## Build order (incremental; Pico A/B tracks can interleave)
1. Toolchain + chip-info sketch on both Picos; repo CLAUDE.md; `kvm_pins.h` + `kvm_config.h`; build-guide update.
2. B: fixed HID descriptors with synthetic jiggle + keypress (on the gaming PC's second port).
3. A: PIO-USB host descriptor/report dump through the hub (hub, keyboard, mouse, dongle; LS-behind-hub risk).
4. `hid_parser` + normalize, with host-side tests on the descriptors captured in step 3.
5. A: passthrough to the gaming PC with a latency histogram.
6. UART link A↔B through the ADuM1201: link probe (baud sweep), protocol, heartbeat.
7. Switch controller: button, release-all, held-key suppression, LED sync, remote wakeup.
8. Relays/solenoids: relay probe, masked pulse, lockout, press-log flash ring, console. **Test the DP switch power-on behavior** (power-cycle 5×, note the port) → set `DP_POWERON_MODE`.
9. Soak: fast clicks, drags across a switch, button spam, rail power cut mid-use, Pico B unplug/replug.
10. SOF phase lock, with before/after latency histograms.

## Risks / hardware flags
- 5V relay module vs. 3.3V GPIO: settled by the relay probe in step 8.
- ADuM1201 grade or clone: settled by marking + the link probe in step 6.
- PIO-USB with a low-speed device behind the hub, and odd dongle descriptors: step 3.
- SOF phase lock: no library fork needed. Pico-PIO-USB 0.7.3 supports `skip_alarm_pool = true` + calling `pio_usb_host_frame()` ourselves, which hands frame timing to core 1. The remaining risk is whether the hub tolerates frame-phase slewing; the slew rate is configurable and gets measured in step 10. Keyboard overpolling was considered and rejected (not needed).

## Verification
- Host-side unit tests (`firmware/tests`, g++): parser, protocol, press-log replay including a simulated torn write.
- On-device: latency histogram on CDC (host-cb → report-complete), PING/PONG RTT; target ≤1.2 ms worst-case added (≤0.3 ms with phase lock).
- On the test bench:
  - a switch mid-drag leaves no stuck button
  - a switch while holding Shift leaves no ghost Shift
  - button spam keeps the DP switches consistent with `state`
  - a rail power cut + restore puts both monitors and input on the gaming PC
  - unplugging Pico B releases everything and recovers
