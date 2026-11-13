#if defined(ESP32) && defined(RX_CAPTURE)

#include "DriveMode.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>
#include "MultiTransportCompanionInterface.h"
#include "RxCapture.h"
#include "RxTransport.h"

extern void uiDriveModeFreeHeavy();
extern void uiDriveModeSetLocked(bool locked);

namespace {
MultiTransportCompanionInterface* s_si = nullptr;
bool s_active = false;

// mbedTLS allocates from internal RAM in this SDK. A TLS session needs two
// 16 KB record buffers plus the handshake's working memory, which did not fit
// in the 54 KB that drive mode frees (measured: the handshake ran the heap down
// to 24 bytes and failed). In drive mode its allocations go to PSRAM instead.
// free is heap_caps_free either way, so a block allocated under one setting can
// be released under the other.
void* tlsCallocPsram(size_t n, size_t size) {
  void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
// What the SDK does by default (CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC).
void* tlsCallocInternal(size_t n, size_t size) {
  return heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
void tlsFree(void* p) { heap_caps_free(p); }

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
  mbedtls_platform_set_calloc_free(tlsCallocPsram, tlsFree);
  rxTransport.start();
  s_active = true;
  return true;
}

void DriveMode::leave() {
  if (!s_active) return;
  s_active = false;
  rxTransport.stop();   // before BLE comes back: the TLS session and NimBLE do not fit together
  rxCapture.stop();     // a live record in flight goes to the queue; the queue file is closed
  mbedtls_platform_set_calloc_free(tlsCallocInternal, tlsFree);
  uiDriveModeSetLocked(false);
  // The MQTT task's stack is only returned to the heap when the idle task runs.
  // NimBLE needs that memory: without this pause its init failed with
  // "BLE_INIT: Malloc failed" and BLE stayed off after drive mode.
  delay(200);
  logHeap("drive: before restore");
  s_si->restoreAfterDriveMode();
  logHeap("drive: after leave");
}

#endif
