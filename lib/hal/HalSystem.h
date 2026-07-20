#pragma once

#include <cstdint>
#include <string>

namespace HalSystem {
struct StackFrame {
  uint32_t sp;
  uint32_t spp[8];
};

void begin();

// Dump panic info to SD card if necessary
void checkPanic();
void clearPanic();

std::string getPanicInfo(bool full = false);
bool isRebootFromPanic();

// Resolve the next OTA app partition relative to the currently running app.
// Returns false when the device has no alternate OTA partition.
bool getAlternateOtaAppIndex(uint8_t& appIndex);

// Select the alternate OTA app as the boot partition and restart immediately.
// Returns false without restarting when the alternate image cannot be selected.
bool switchToAlternateOtaApp();
}  // namespace HalSystem
