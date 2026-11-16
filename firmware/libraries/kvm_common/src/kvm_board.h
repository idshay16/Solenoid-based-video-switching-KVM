// kvm_board.h — which physical Pico is this? (unique flash ID, see CLAUDE.md)
// Lets a sketch pick its role automatically and lets the main firmware warn if
// it was flashed onto the wrong board.
#pragma once
#include <stdint.h>

namespace kvm {
namespace board {

constexpr const char *ID_PICO_A = "5B3009CC81C08F23";
constexpr const char *ID_PICO_B = "80A2839148DA51D9";

enum Role : uint8_t { UNKNOWN = 0, A = 'A', B = 'B' };

const char *id();   // 16 hex chars
Role role();        // by ID; UNKNOWN for any other board

}  // namespace board
}  // namespace kvm
