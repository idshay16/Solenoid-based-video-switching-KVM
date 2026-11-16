// router.h — Pico A core 1: physical input -> InputModel -> active PC.
#pragma once
#include <stdint.h>

namespace router {

void begin();   // setup1()
void task();    // loop1()

// Requests from core 0 (handled on core 1's next loop).
void requestDescribe();

uint32_t reportsIn();

}  // namespace router
