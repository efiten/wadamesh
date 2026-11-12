// SPDX-License-Identifier: GPL-3.0-or-later
#include "RxGate.h"

#include <math.h>

double rxDistanceM(const RxFix& a, const RxFix& b) {
  static const double kR = 6371000.0;
  static const double kRad = 3.14159265358979323846 / 180.0;
  const double dlat = (b.lat - a.lat) * kRad;
  const double dlon = (b.lon - a.lon) * kRad;
  const double s1 = sin(dlat / 2), s2 = sin(dlon / 2);
  const double h = s1 * s1 + cos(a.lat * kRad) * cos(b.lat * kRad) * s2 * s2;
  return 2 * kR * asin(sqrt(h));
}

// motion.js updateMotion(): leaving the radius moves the anchor and restarts the
// dwell; staying inside it for the dwell time means parked.
void RxGate::onFix(const RxFix& fix, uint32_t now_ms) {
  _has_fix = true;
  _fix = fix;
  if (!_have_anchor || rxDistanceM(_anchor, fix) > kStationaryRadiusM) {
    _have_anchor = true;
    _anchor = fix;
    _anchor_ms = now_ms;
    _stationary = false;
    return;
  }
  _stationary = (uint32_t)(now_ms - _anchor_ms) >= kStationaryDwellMs;
}

bool RxGate::rfSampleDue(uint32_t now_ms) {
  if (!_has_fix || !_clock_valid) return false;   // a sample without a position is not sent
  bool due = !_have_rf;
  if (!due) {
    const uint32_t age = (uint32_t)(now_ms - _rf_ms);
    due = _stationary ? age >= kRfParkedMs
                      : (age >= kRfMovingMs || rxDistanceM(_rf_fix, _fix) >= kRfStepM);
  }
  if (!due) return false;
  _have_rf = true;
  _rf_fix = _fix;
  _rf_ms = now_ms;
  return true;
}

bool RxGate::discoverDue(uint32_t now_ms) {
  if (!captureAllowed()) return false;
  if (_have_discover && (uint32_t)(now_ms - _discover_ms) < kDiscoverMs) return false;
  if (_have_traffic && (uint32_t)(now_ms - _traffic_ms) < kDiscoverBackoffMs) return false;
  _have_discover = true;
  _discover_ms = now_ms;
  return true;
}

void RxGate::onTrafficHeard(uint32_t now_ms) {
  _have_traffic = true;
  _traffic_ms = now_ms;
}
