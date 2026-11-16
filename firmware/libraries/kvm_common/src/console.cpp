#include "console.h"
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <string.h>
#include "kvm_log.h"

namespace kvm {
namespace console {

namespace {
const Command *g_cmds = nullptr;
uint8_t g_n = 0;
const char *g_banner = "";
char g_line[96];
uint8_t g_len = 0;
bool g_was_open = false;

void dispatch(char *line) {
  char *argv[8];
  int argc = 0;
  for (char *p = strtok(line, " \t"); p && argc < 8; p = strtok(nullptr, " \t")) argv[argc++] = p;
  if (!argc) return;
  if (!strcmp(argv[0], "help") || !strcmp(argv[0], "h") || !strcmp(argv[0], "?")) { printHelp(); return; }
  for (uint8_t i = 0; i < g_n; ++i) {
    if (!strcmp(argv[0], g_cmds[i].name)) {
      g_cmds[i].fn(argc, argv);
      return;
    }
  }
  log::printf("unknown command '%s' (help)\n", argv[0]);
}
}  // namespace

void begin(const Command *cmds, uint8_t n, const char *banner) {
  g_cmds = cmds;
  g_n = n;
  g_banner = banner;
}

void printHelp() {
  log::printf("\n%s\n", g_banner);
  for (uint8_t i = 0; i < g_n; ++i) log::printf("  %-10s %s\n", g_cmds[i].name, g_cmds[i].help);
}

void loop() {
  const bool open = (bool)Serial;
  if (open && !g_was_open) printHelp();
  g_was_open = open;
  if (!open) return;
  log::drain(Serial, 256);
  while (Serial.available()) {
    int c = Serial.read();
    if (c == '\r' || c == '\n') {
      if (g_len) {
        g_line[g_len] = 0;
        log::printf("> %s\n", g_line);
        dispatch(g_line);
        g_len = 0;
      }
    } else if (c == 8 || c == 127) {
      if (g_len) g_len--;
    } else if (g_len < sizeof g_line - 1 && c >= 32) {
      g_line[g_len++] = (char)c;
    }
  }
}

}  // namespace console
}  // namespace kvm
