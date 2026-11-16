// Bring-up step 1: chip info. Flash to BOTH Picos (no wiring needed).
//
// Prints once per second over USB serial:
//   - SYSINFO chip ID (part, revision -> RP2350 stepping), package
//   - flash JEDEC ID and size, unique board ID
//   - system clock
//   - VSYS voltage (GP29 ADC) and whether USB VBUS is present (GP24)
//   - every GPIO this board uses per kvm_pins.h, with its header pin number
//
// Open the serial monitor at any baud (USB CDC ignores it):
//   arduino-cli monitor -p COMx
//
// What to check:
//   - stepping: A2/A3 => see the E9 note on USB_HOST_DP in kvm_pins.h
//   - flash size: should be 4 MB on a Pico 2
//   - Pico A on the 5V rail: VSYS ~4.7-5.0 V (rail minus a small diode drop)

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>   // required with usbstack=tinyusb: provides Serial (CDC)
#include <kvm_pins.h>
#include <kvm_config.h>
#include "hardware/structs/sysinfo.h"
#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/unique_id.h"

static uint32_t read_flash_jedec() {
  uint8_t tx[4] = {0x9F, 0, 0, 0};
  uint8_t rx[4] = {0};
  uint32_t irq = save_and_disable_interrupts();
  flash_do_cmd(tx, rx, sizeof tx);   // runs from RAM, safe while XIP is paused
  restore_interrupts(irq);
  return (uint32_t(rx[1]) << 16) | (uint32_t(rx[2]) << 8) | rx[3];
}

static void print_pin(const char *name, int gp) {
  Serial.printf("    %-14s GP%-2d  header pin %u\n", name, gp, kvm::header_pin_of(gp));
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  pinMode(24, INPUT);                 // VBUS sense
  pinMode(kvm::ONBOARD_LED, OUTPUT);
}

void loop() {
  static uint32_t n = 0;
  digitalWrite(kvm::ONBOARD_LED, n & 1);

  const uint32_t chip_id = sysinfo_hw->chip_id;
  const uint32_t manuf   = chip_id & 0xFFFu;
  const uint32_t part    = (chip_id >> 12) & 0xFFFFu;
  const uint32_t rev     = chip_id >> 28;
  const uint32_t pkg     = sysinfo_hw->package_sel;

  const uint32_t jedec = read_flash_jedec();
  const uint8_t  cap   = jedec & 0xFF;
  const uint32_t flash_bytes = (cap >= 0x10 && cap <= 0x20) ? (1u << cap) : 0;

  char uid[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
  pico_get_unique_board_id_string(uid, sizeof uid);

  const float vsys = analogRead(A3) * 3.3f / 4095.0f * 3.0f;   // GP29 = VSYS/3
  const bool vbus  = digitalRead(24);

  Serial.printf("\n=== KVM chip info #%lu ===\n", (unsigned long)n++);
  Serial.printf("  CHIP_ID   0x%08lx  (part 0x%04lx, manuf 0x%03lx, revision %lu)\n",
                (unsigned long)chip_id, (unsigned long)part, (unsigned long)manuf, (unsigned long)rev);
  Serial.printf("  package   %s (PACKAGE_SEL=%lu)\n", (pkg & 1) ? "QFN60 / RP2350A" : "QFN80 / RP2350B",
                (unsigned long)pkg);
  Serial.printf("  flash     JEDEC 0x%06lx  ->  %lu KB\n", (unsigned long)jedec,
                (unsigned long)(flash_bytes / 1024));
  Serial.printf("  board id  %s\n", uid);
  Serial.printf("  sysclk    %lu MHz\n", (unsigned long)(clock_get_hz(clk_sys) / 1000000));
  Serial.printf("  VSYS      %.2f V   USB VBUS %s\n", vsys, vbus ? "present" : "absent");
  Serial.printf("  link baud %lu (KVM_LINK_BAUD)\n", (unsigned long)KVM_LINK_BAUD);
  Serial.println("  Pico A pins (kvm_pins.h):");
  print_pin("LINK_TX", kvm::pico_a::LINK_TX);
  print_pin("LINK_RX", kvm::pico_a::LINK_RX);
  print_pin("RELAY_A", kvm::pico_a::RELAY_A);
  print_pin("RELAY_B", kvm::pico_a::RELAY_B);
  print_pin("SWITCH_BTN", kvm::pico_a::SWITCH_BTN);
  print_pin("USB_HOST_DP", kvm::pico_a::USB_HOST_DP);
  print_pin("USB_HOST_DM", kvm::pico_a::USB_HOST_DM);
  Serial.println("  Pico B pins (kvm_pins.h):");
  print_pin("LINK_TX", kvm::pico_b::LINK_TX);
  print_pin("LINK_RX", kvm::pico_b::LINK_RX);

  delay(1000);
}
