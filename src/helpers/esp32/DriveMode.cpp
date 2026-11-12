#if defined(ESP32) && defined(RX_CAPTURE)

#include "DriveMode.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include "MultiTransportCompanionInterface.h"

extern void uiDriveModeFreeHeavy();
extern void uiDriveModeSetLocked(bool locked);

namespace {
MultiTransportCompanionInterface* s_si = nullptr;
bool s_active = false;

void logHeap(const char* phase) {
  char line[160];
  DriveMode::formatHeap(line, sizeof(line), phase);
  Serial.println(line);
}
}

void DriveMode::formatHeap(char* out, size_t cap, const char* phase) {
  const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  snprintf(out, cap, "[heap] %s: internal free=%u largest=%u low=%u dma=%u psram=%u",
           phase,
           (unsigned)heap_caps_get_free_size(caps),
           (unsigned)heap_caps_get_largest_free_block(caps),
           (unsigned)heap_caps_get_minimum_free_size(caps),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void DriveMode::attach(MultiTransportCompanionInterface* si) { s_si = si; }

bool DriveMode::active() { return s_active; }

bool DriveMode::enter() {
  if (s_active) return true;
  if (!s_si) return false;
  logHeap("drive: before");
  s_si->suspendForDriveMode();
  logHeap("drive: after ble+servers");
  uiDriveModeFreeHeavy();
  uiDriveModeSetLocked(true);
  logHeap("drive: after ui");
  s_active = true;
  return true;
}

void DriveMode::leave() {
  if (!s_active) return;
  s_active = false;
  uiDriveModeSetLocked(false);
  s_si->restoreAfterDriveMode();
  logHeap("drive: after leave");
}

#endif
