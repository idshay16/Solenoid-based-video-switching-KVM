#include "kvm_board.h"
#include <Arduino.h>
#include <pico/unique_id.h>
#include <string.h>

namespace kvm {
namespace board {

const char *id() {
  static char buf[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1] = {0};
  if (!buf[0]) pico_get_unique_board_id_string(buf, sizeof buf);
  return buf;
}

Role role() {
  const char *s = id();
  if (!strcmp(s, ID_PICO_A)) return A;
  if (!strcmp(s, ID_PICO_B)) return B;
  return UNKNOWN;
}

}  // namespace board
}  // namespace kvm
