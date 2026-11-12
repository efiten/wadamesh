// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// RX-capture records as CoreScope's ingestor expects them. The field names and
// types are the contract coredrive-rx publishes (its src/publisher.js,
// buildPayload and buildRfPayload). No Arduino dependency: host-tested by
// test/rxcapture/run.sh.

#include <stddef.h>
#include <stdint.h>

struct RxFix {
  double lat, lon;
  float acc_m;
};

struct RxPacket {
  uint32_t epoch;        // UTC seconds
  float snr;
  int rssi;
  const uint8_t* raw;
  int len;
  RxFix fix;
};

struct RxRfSample {
  uint32_t epoch;
  RxFix fix;
  bool stationary;
  uint32_t uptime_secs;
  uint16_t battery_mv;
  uint8_t queue_len;
  uint16_t errors;
  int16_t noise_floor;
  int8_t last_rssi;
  float last_snr;
  uint32_t tx_air_secs, rx_air_secs, recv, sent, flood_rx, direct_rx, flood_tx, direct_tx, recv_errors;
};

// Each returns the JSON length (without the NUL), or 0 when it does not fit in cap.
// The output never contains a newline. An empty name omits the "origin" key.
size_t rxBuildPacketJson(char* out, size_t cap, const char* pubkey_hex, const char* name, const RxPacket& p);
size_t rxBuildRfJson(char* out, size_t cap, const char* pubkey_hex, const char* name, const RxRfSample& s);

// kind: 'p' = packets, 'r' = rf. Returns the topic length, or 0 for an unknown kind or a short buffer.
size_t rxTopic(char* out, size_t cap, const char* pubkey_hex, char kind);

void rxIso8601(char out[21], uint32_t epoch);   // "2026-11-12T21:26:17Z"
