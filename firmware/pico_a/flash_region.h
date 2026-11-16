// flash_region.h — the 64 KB "FS" flash area (Tools > Flash Size: FS 64KB) as a
// FlashIf for the press log. Programming/erasing pauses core 1 and disables
// interrupts for the duration (~1 ms per page, ~50 ms per sector).
#pragma once
#include <presslog.h>

kvm::FlashIf &flashRegion();
