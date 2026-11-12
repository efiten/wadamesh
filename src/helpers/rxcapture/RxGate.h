// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Decides when RX capture records, samples and transmits. Timings are the ones
// coredrive-rx uses (its src/motion.js, src/rfstats.js and docs/user-guide.md).
// No Arduino dependency: host-tested by test/rxcapture/run.sh.
//
// Every now_ms is a free-running millisecond counter that may wrap.

#include <stdint.h>

#include "RxRecord.h"

double rxDistanceM(const RxFix& a, const RxFix& b);   // haversine, R = 6371000 m

class RxGate {
public:
  static constexpr double   kStationaryRadiusM = 75.0;      // motion.js DEFAULT_RADIUS_M
  static constexpr uint32_t kStationaryDwellMs = 300000;    // motion.js DEFAULT_DWELL_MS
  static constexpr double   kRfStepM           = 100.0;     // rfstats.js RF_STEP_M
  static constexpr uint32_t kRfMovingMs        = 15000;     // rfstats.js MOVING_MS
  static constexpr uint32_t kRfParkedMs        = 300000;    // rfstats.js PARKED_MS
  static constexpr uint32_t kDiscoverMs        = 30000;     // user-guide.md: at most every 30 s
  static constexpr uint32_t kDiscoverBackoffMs = 15000;     // user-guide.md: waits 15 s after traffic

  void onFix(const RxFix& fix, uint32_t now_ms);
  void onNoFix() { _has_fix = false; }
  // False until the clock holds a real time: a record stamped 1970 is worse than none.
  void setClockValid(bool valid) { _clock_valid = valid; }

  // A reception may be recorded: there is a fix, the clock is real and the device moves.
  bool captureAllowed() const { return _has_fix && _clock_valid && !_stationary; }
  bool stationary() const { return _stationary; }

  // Each returns true once per due event and then arms the next one.
  bool rfSampleDue(uint32_t now_ms);
  bool discoverDue(uint32_t now_ms);
  void onTrafficHeard(uint32_t now_ms);

private:
  bool _has_fix = false;
  bool _clock_valid = false;
  RxFix _fix{};

  bool _have_anchor = false;
  RxFix _anchor{};
  uint32_t _anchor_ms = 0;
  bool _stationary = false;

  bool _have_rf = false;
  RxFix _rf_fix{};
  uint32_t _rf_ms = 0;

  bool _have_discover = false;
  uint32_t _discover_ms = 0;
  bool _have_traffic = false;
  uint32_t _traffic_ms = 0;
};
