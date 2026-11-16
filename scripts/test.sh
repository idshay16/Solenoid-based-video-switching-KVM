#!/bin/sh
# Host-side unit tests for the pure-C++ parts of kvm_common.
# Runs anywhere with g++ (Linux, macOS, WSL). On Windows: scripts\test.ps1.
set -e
cd "$(dirname "$0")/.."
SRC=firmware/libraries/kvm_common/src
OUT=firmware/tests/bin
mkdir -p "$OUT"
g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$SRC" -Ifirmware/tests \
  firmware/tests/*.cpp \
  "$SRC/hid_parser.cpp" "$SRC/input_model.cpp" "$SRC/hid_out.cpp" \
  $(ls "$SRC/link_proto.cpp" "$SRC/presslog.cpp" "$SRC/switch_logic.cpp" "$SRC/sof_pll.cpp" 2>/dev/null) \
  -o "$OUT/kvm_tests"
"$OUT/kvm_tests"
