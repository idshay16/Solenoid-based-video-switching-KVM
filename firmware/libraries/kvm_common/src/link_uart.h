// link_uart.h — the UART side of the A<->B link (pico-sdk hardware_uart, not
// Arduino Serial1/2, so nothing else buffers or blocks).
//
//  * send(): frames go into a 1 KB TX ring and straight into the 32-byte
//    hardware FIFO; a typical 10-15 byte frame leaves immediately. Never blocks;
//    if the ring is full the frame is dropped and counted.
//  * RX: either an IRQ fills a ring (Pico B), or poll() drains the FIFO from a
//    loop (Pico A core 1, which owns the UART). recv() returns decoded frames.
// One link per firmware; call everything from one core (IRQ excepted).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "link_proto.h"

namespace kvm {
namespace linkuart {

// uart_idx 0/1, pins from kvm_pins.h. Returns the actual baud rate.
uint32_t begin(uint8_t uart_idx, uint8_t tx_pin, uint8_t rx_pin, uint32_t baud, bool rx_irq);
uint32_t setBaud(uint32_t baud);   // waits for TX to drain first; returns actual
uint32_t baud();

bool send(uint8_t type, const void *payload = nullptr, size_t len = 0);

// Move TX ring -> FIFO and (without IRQ) RX FIFO -> decoder input. Call often.
void poll();

// Next valid frame, if any. Pointers stay valid until the next recv().
bool recv(uint8_t &type, const uint8_t *&payload, size_t &len);

bool txIdle();                 // ring empty and FIFO/shift register done
const link::Receiver &rx();
uint32_t txDropped();
uint32_t uartErrors();         // UART-level framing / overrun / break errors
uint32_t rxDropped();          // RX ring overflow (IRQ mode)
void resetStats();

}  // namespace linkuart
}  // namespace kvm
