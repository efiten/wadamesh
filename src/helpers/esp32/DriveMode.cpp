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
extern void uiDriveModeAlert(const char* text);

namespace {
MultiTransportCompanionInterface* s_si = nullptr;
bool s_active = false;
int8_t s_request = -1;            // -1 none, 0 leave, 1 enter
uint32_t s_ble_check_ms = 0;      // millis() to check that BLE came back, 0 = nothing pending
bool s_ble_retried = false;
const uint32_t kBleCheckMs = 3000;
// The cold-start guard in MultiTransportCompanionInterface::enableBle().
const size_t kBleNeedFree = 50u * 1024u, kBleNeedBlock = 20u * 1024u;

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
  s_ble_check_ms = 0;
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
  // NimBLE needs that memory: without a pause its init failed with
  // "BLE_INIT: Malloc failed" and BLE stayed off after drive mode. Wait until
  // the heap is back above what the BLE cold start asks for, at most 1 s.
  {
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t t0 = millis();
    do {
      delay(50);
    } while ((heap_caps_get_free_size(caps) < kBleNeedFree + 1024 ||
              heap_caps_get_largest_free_block(caps) < kBleNeedBlock) &&
             (uint32_t)(millis() - t0) < 1000);
  }
  logHeap("drive: before restore");
  s_si->restoreAfterDriveMode();
  logHeap("drive: after leave");
  s_ble_check_ms = millis() + kBleCheckMs;
  s_ble_retried = false;
}

void DriveMode::request(bool on) { s_request = on ? 1 : 0; }

void DriveMode::loop() {
  if (s_request >= 0) {
    const bool on = s_request == 1;
    s_request = -1;
    if (on) { if (!enter()) uiDriveModeAlert("Could not start drive mode"); }
    else leave();
  }
  // BLE can be refused after a drive (heap guard) or deferred (Wi-Fi is still
  // associating). Try once more, then tell the user instead of staying silent.
  if (s_ble_check_ms && !s_active && (int32_t)(millis() - s_ble_check_ms) >= 0) {
    s_ble_check_ms = 0;
    if (s_si && s_si->bleMissingAfterDriveMode()) {
      if (!s_ble_retried) {
        s_ble_retried = true;
        s_si->enableBle();
        s_ble_check_ms = millis() + kBleCheckMs;
      } else {
        Serial.println("drive: BLE did not come back after drive mode");
        uiDriveModeAlert("Bluetooth did not restart; toggle it in Settings");
      }
    }
  }
}

#endif
