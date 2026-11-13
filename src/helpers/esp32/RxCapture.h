// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#if defined(ESP32) && defined(RX_CAPTURE)

// RX capture: while drive mode is on, every packet the radio hears is tagged with
// the GNSS position and published to the CoreScope broker (RxTransport), together
// with periodic RF samples. Without a broker session the records go to a file
// queue (RxQueue) and are sent, oldest first, once the session is back.

#include <Arduino.h>

#include "RxTransport.h"

struct RxCaptureStatus {
  bool drive; bool fix; float acc_m; bool stationary; RxLink link;
  uint32_t captured, sent, pending, dropped; float last_snr; bool sd;
};

class RxCapture {
public:
  void begin();
  void loop();
  void stop();   // drive mode is ending: settle the record in flight and close the queue file
  // Called from MyMesh::logRxRaw. Copies the packet into a ring and returns.
  void onRxRaw(float snr, float rssi, const uint8_t* raw, int len);
  void getStatus(RxCaptureStatus& out) const;
  void formatStatus(char* out, size_t cap) const;
};

extern RxCapture rxCapture;
bool rxCaptureHoldsSd();   // the queue file is open on the SD card

#endif
