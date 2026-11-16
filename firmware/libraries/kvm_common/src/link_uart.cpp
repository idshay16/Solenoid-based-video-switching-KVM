#include "link_uart.h"
#include <Arduino.h>
#include <hardware/gpio.h>
#include <hardware/irq.h>
#include <hardware/uart.h>

namespace kvm {
namespace linkuart {

namespace {
uart_inst_t *g_uart = nullptr;
uint32_t g_baud = 0;
bool g_irq = false;

uint8_t g_tx[1024];
volatile uint32_t g_tx_head = 0, g_tx_tail = 0;

uint8_t g_rx[2048];
volatile uint32_t g_rx_head = 0, g_rx_tail = 0;

link::Receiver g_rcv;
volatile uint32_t g_tx_dropped = 0, g_uart_err = 0, g_rx_dropped = 0;

inline void __not_in_flash_func(drainTx)() {
  while (g_tx_tail != g_tx_head && uart_is_writable(g_uart)) {
    uart_get_hw(g_uart)->dr = g_tx[g_tx_tail & (sizeof g_tx - 1)];
    g_tx_tail = g_tx_tail + 1;
  }
}

// Read every byte waiting in the hardware FIFO into the RX ring.
inline void __not_in_flash_func(drainRxFifo)() {
  uart_hw_t *hw = uart_get_hw(g_uart);
  while (!(hw->fr & UART_UARTFR_RXFE_BITS)) {
    const uint32_t dr = hw->dr;
    if (dr & (UART_UARTDR_FE_BITS | UART_UARTDR_OE_BITS | UART_UARTDR_BE_BITS | UART_UARTDR_PE_BITS))
      g_uart_err = g_uart_err + 1;
    if (g_rx_head - g_rx_tail < sizeof g_rx) {
      g_rx[g_rx_head & (sizeof g_rx - 1)] = (uint8_t)dr;
      g_rx_head = g_rx_head + 1;
    } else {
      g_rx_dropped = g_rx_dropped + 1;
    }
  }
}

void __not_in_flash_func(onUartIrq)() { drainRxFifo(); }
}  // namespace

uint32_t begin(uint8_t idx, uint8_t tx_pin, uint8_t rx_pin, uint32_t baud, bool rx_irq) {
  g_uart = idx ? uart1 : uart0;
  g_irq = rx_irq;
  g_baud = uart_init(g_uart, baud);
  uart_set_format(g_uart, 8, 1, UART_PARITY_NONE);
  uart_set_hw_flow(g_uart, false, false);
  uart_set_fifo_enabled(g_uart, true);
  gpio_set_function(tx_pin, GPIO_FUNC_UART);
  gpio_set_function(rx_pin, GPIO_FUNC_UART);
  // Idle-high line: a pull-up keeps RX quiet while the other side (or the
  // isolator's far side) is unpowered, instead of reading noise as breaks.
  gpio_pull_up(rx_pin);
  if (rx_irq) {
    const int irq = idx ? UART1_IRQ : UART0_IRQ;
    irq_set_exclusive_handler(irq, onUartIrq);
    irq_set_enabled(irq, true);
    uart_set_irq_enables(g_uart, true, false);   // RX + RX-timeout
  }
  return g_baud;
}

uint32_t setBaud(uint32_t baud) {
  const uint32_t t0 = millis();
  while (!txIdle() && millis() - t0 < 50) drainTx();
  g_baud = uart_set_baudrate(g_uart, baud);
  return g_baud;
}

uint32_t baud() { return g_baud; }

bool __not_in_flash_func(send)(uint8_t type, const void *payload, size_t len) {
  uint8_t wire[link::kMaxWire];
  const size_t n = link::buildFrame(type, payload, len, wire);
  if (!n) return false;
  if (sizeof g_tx - (g_tx_head - g_tx_tail) < n) {
    g_tx_dropped = g_tx_dropped + 1;
    return false;
  }
  for (size_t i = 0; i < n; ++i) g_tx[(g_tx_head + i) & (sizeof g_tx - 1)] = wire[i];
  g_tx_head = g_tx_head + n;
  drainTx();
  return true;
}

void __not_in_flash_func(poll)() {
  drainTx();
  if (!g_irq) drainRxFifo();
}

bool __not_in_flash_func(recv)(uint8_t &type, const uint8_t *&payload, size_t &len) {
  if (!g_irq) drainRxFifo();
  while (g_rx_tail != g_rx_head) {
    const uint8_t b = g_rx[g_rx_tail & (sizeof g_rx - 1)];
    g_rx_tail = g_rx_tail + 1;
    if (g_rcv.feed(b)) {
      type = g_rcv.type();
      payload = g_rcv.payload();
      len = g_rcv.len();
      return true;
    }
  }
  return false;
}

bool txIdle() { return g_tx_tail == g_tx_head && !(uart_get_hw(g_uart)->fr & UART_UARTFR_BUSY_BITS); }
const link::Receiver &rx() { return g_rcv; }
uint32_t txDropped() { return g_tx_dropped; }
uint32_t uartErrors() { return g_uart_err; }
uint32_t rxDropped() { return g_rx_dropped; }
void resetStats() {
  g_rcv.resetStats();
  g_tx_dropped = g_uart_err = g_rx_dropped = 0;
}

}  // namespace linkuart
}  // namespace kvm
