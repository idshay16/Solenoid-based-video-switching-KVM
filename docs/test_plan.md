# Hardware test plan

Steps 2–10 were written and compiled without hardware attached. The pure logic has host-side unit tests (`.\scripts\test.ps1`): the parser, input model, output queue, link protocol, switch timing, press log and fuzzing. Everything below is what still needs a real board.

Work through it in order. Each step's sketch only needs the hardware listed for that step. Flash with the Arduino IDE (settings in `CLAUDE.md`) or with `.\scripts\build.ps1 <sketch> -Port COMx`.

Board IDs: **Pico A** = `5B3009CC81C08F23`, **Pico B** = `80A2839148DA51D9`. The main firmwares print a warning if they're flashed onto the wrong board.

For each step, send back the console output it asks for, plus pass/fail. If something fails, check out the commit before that step to get back to a known-good state.

---

## Step 2: Pico B as a HID device (`bringup/02_hid_device`)
Hardware: Pico B → a second USB port on the gaming PC.

| # | Do | Pass |
|---|---|---|
| 1 | Device Manager | New keyboard, HID mouse and consumer device, no ⚠. The `h` command prints `USB mounted: yes`. |
| 2 | `m` | The cursor draws a ~100 px square. |
| 3 | `k`, then click into Notepad within 3 s | `kvm` is typed. |
| 4 | `v` | The volume popup goes up, then down. |
| 5 | `r` | About 1000 reports/s, avg ≈ 1000 µs, histogram mostly 750–1249. |
| 6 | Caps Lock on the real keyboard ×2 | The Pico LED follows, and an `LEDs:` line prints. |
| 7 | Unplug and replug | Still works. |

## Step 3: Pico A USB host dump (`bringup/03_host_dump`)
Hardware: Pico A with the USB-A pigtail (D+ GP16 pin 21, D− GP17 pin 22, GND). Power the hub from the 5V rail. Keyboard, mouse cable and dongle go into the hub. Pico A's USB-C goes to the PC.

- **Pass:**
  - `DEV` lines appear for the hub and every device, with VID/PID, `speed`, and `hub/port`.
  - The hub line says `single-TT` or `multi-TT`.
  - Each device has `HID MOUNT ... (active protocol: report)` and an `RD` hex dump.
  - `R` lines appear when you press keys or move the mouse.
- **`s` after 5 s of fast mouse movement:** the mouse interface shows ~1000/s (or whatever its bInterval allows).
- **Watch for:** a low-speed device (`speed low`) behind the hub. This was the main PIO-USB risk; confirm its reports arrive.
- **Send back:** the whole boot dump. Also send a few `R` lines each for: one key, mouse move, each mouse button, wheel, and a media key. These become test vectors in `firmware/tests/test_hid_parser.cpp`.

## Step 4: Parser on real devices (`bringup/04_parser_live`)
Same hardware as step 3.

- Each interface prints one `host:` line naming what it is: `kbd`, `mouse`, `consumer`, `leds`.
  - There should be no `descriptor unusable` lines.
  - If one says `TRUNCATED`, send its RD dump.
- `KBD`, `BTN`, `MOVE`, `CONS` lines match what you do:
  - every mouse button including the side buttons
  - wheel and tilt
  - media keys
  - all of this on **both** the cable mouse and the dongle
- Hold 8 keys: `KBD` shows the 6 newest.
- `l`: the keyboard's Num, Caps and Scroll LEDs light in turn.
- `s` while holding Shift: Shift disappears from `KBD` until you release it and press it again.

## Step 5: Passthrough to the gaming PC (`pico_a`)
Hardware: as in step 3. Pico B and the link aren't needed.

- Keyboard and mouse work normally on the gaming PC through Pico A, including games, the BIOS keyboard and media keys.
- Caps Lock on the gaming PC lights the physical keyboard's LED.
- `lat` after a minute of mouse movement:
  - Expected: mouse avg roughly 400–600 µs, max under ~1100 µs.
  - Send back the histogram.
- Put the PC to sleep, then move the mouse or press a key: it wakes. `stats` shows the wakeup count.

## Step 6: UART link (`bringup/06_link_probe`, then `pico_b` + `pico_a`)
1. **Single-board test, no isolator needed:**
   - Flash 06 to Pico A.
   - Jumper GP4 (pin 6) to GP5 (pin 7).
   - Run `loop`.
   - Pass: 1 Mbaud is clean. Note how high it stays clean.
2. **Wire the ADuM1201** as described in the header of `06_link_probe.ino`. Each side is powered by its own Pico's 3V3, and the GNDs are never joined. Also read the chip marking: `ADuM1201AR`, `BR` or `CR`.
3. Flash 06 to both Picos. Run `probe` on A.
   - **Send back:** the table and the recommended baud. Set `KVM_LINK_BAUD` in `kvm_config.h`.
4. Flash `pico_b` to B and `pico_a` to A.
   - `link` on A shows `UP`.
   - `ping 1000` shows 0 lost. Send back the RTT.
5. Unplug Pico B: A logs `B lost` within ~150 ms. Replug: `B up`.

## Step 7: Switching, input only (`pico_a` + `pico_b`)
Test bench: both Picos on the gaming PC. Switching moves input between "Pico A's keyboard/mouse" and "Pico B's keyboard/mouse". Both drive the same cursor, so watch `state`, and the Device Manager / `link` output on B.

| Test | Pass |
|---|---|
| `switch` (or the lid button: GP15 pin 20 to GND) | `switch -> PC B`. Typing still works (now through B). `lat` on B shows the B-side histogram. |
| Hold Shift, switch, keep holding, type `a` | Lowercase `a` (no ghost Shift). Release Shift, press it again: Shift works. |
| Drag a window, switch mid-drag | The drag ends cleanly at the moment of the switch; no stuck button. |
| Caps Lock state | Switching shows the active PC's lock LEDs on the keyboard. |
| Hammer the button 5× quickly | One switch, plus at most one pending toggle (`state`). |
| Pull Pico B's USB while holding a key on B | Nothing stuck after replug. |

Optional hotkey: build with `KVM_HOTKEY_ENABLED 1` (edit `kvm_config.h` in the IDE). Scroll Lock then switches, and neither PC sees the Scroll Lock press.

## Step 8: Relays, solenoids, DP switches (`bringup/08_relay_probe`, then `pico_a`)
Follow the procedure in the header of `08_relay_probe.ino`. It covers:
1. **Polarity and drive mode:** sets `KVM_RELAY_ACTIVE_LOW` and `KVM_RELAY_OPEN_DRAIN`.
2. **Minimum reliable pulse:** set `KVM_DP_PULSE_MS` to about 1.5× that.
3. **`burst 20`:** Pico A must not reboot. If it does, set `KVM_DP_FIRE_STAGGER_MS 30`.
4. **DP power-on test, 5×:** sets `KVM_DP_POWERON_MODE`.

Then, with `pico_a`:
- A relay never clicks at boot, on reset, or when plugging in USB.
- `switch`: both monitors and the input move together.
- `log`: shows `BOOT` and `PRESS` records.
- `nudge a`: only monitor A changes. Running `nudge a` again puts it back.
- Power-cycle the rail:
  - Mode **PORT1:** after the restart, `state` shows both monitors on input 1 and PC A.
  - Mode **REMEMBER:** `state` matches what the monitors actually show.

## Step 9: Soak (`pico_a` + `pico_b`, full box)
- `soak 2`: let it run ≥ 30 minutes while you use the mouse and keyboard. Then run `soak stop`.
  - **Pass:** 0 invariant violations, 0 link drops, monitors still in step with `state`, no watchdog note at boot.
  - `link` on Pico B also shows 0 invariant violations.
- Abuse checks, then confirm nothing is stuck and `state` is correct:
  - fast clicks during switches
  - holding keys across switches
  - button spam
  - unplugging/replugging Pico B
  - unplugging the dongle mid-drag
  - **cutting the 5V rail mid-use**
- **Send back:** the `soak` report.
