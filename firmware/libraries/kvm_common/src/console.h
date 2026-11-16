// console.h — tiny non-blocking line console on the CDC serial port (core 0).
// Commands are "name arg arg..."; output goes through kvm::log (never blocks).
#pragma once
#include <stdint.h>

namespace kvm {
namespace console {

struct Command {
  const char *name;
  const char *help;        // "args - what it does"
  void (*fn)(int argc, char **argv);
};

// `banner` prints when the Serial Monitor is opened (and on "help").
void begin(const Command *cmds, uint8_t n, const char *banner);

// Call from loop(): drains the log ring to Serial and handles input.
void loop();

void printHelp();

}  // namespace console
}  // namespace kvm
