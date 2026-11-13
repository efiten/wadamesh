// SPDX-License-Identifier: GPL-3.0-or-later
#include "RxPump.h"

#include <string.h>

void RxPump::append(const char* line) {
  char* dst = slot(_count);
  strncpy(dst, line, _cap - 1);
  dst[_cap - 1] = 0;
  _count++;
}

// Without a file there is nowhere to move them: they stay in memory.
void RxPump::spill(uint8_t keep) {
  if (!_q || _count <= keep) return;
  for (uint8_t i = keep; i < _count; ++i) _q->push(slot(i));   // a failed push counts in the queue's dropped()
  _count = keep;
  if (_count == 0) _first = 0;
}

void RxPump::settle() {
  if (_link.takeAck()) {
    if (_inflight == InFlight::Memory && _count > 0) {
      _first = (uint8_t)((_first + 1) % _slots);
      _count--;
    } else if (_inflight == InFlight::File && _q) {
      _q->ack();
    }
    _inflight = InFlight::None;
  }
  if (_link.takeFailure()) {
    const bool was_memory = _inflight == InFlight::Memory;
    _inflight = InFlight::None;        // a record from the file simply stays there
    if (was_memory) spill(0);
  }
}

void RxPump::submit(const char* line) {
  const uint8_t in_flight = _inflight == InFlight::Memory ? 1 : 0;
  const bool backlog = _q && _q->pending() > 0;
  if (!backlog && (_link.up() || !_q)) {
    if (_count < _slots) { append(line); return; }
    if (!_q) { _lost++; return; }
  }
  if (!_q) { _lost++; return; }
  spill(in_flight);
  _q->push(line);
}

void RxPump::service() {
  settle();
  if (_inflight == InFlight::None && _count > 0 && !_link.up()) spill(0);
  if (_inflight != InFlight::None || !_link.canPublish()) return;
  if (_q && _q->pending() > 0) {
    if (_q->peek(_scratch, _cap) && _link.publish(_scratch)) _inflight = InFlight::File;
  } else if (_count > 0) {
    if (_link.publish(slot(0))) _inflight = InFlight::Memory;
  }
}

void RxPump::shutdown() {
  settle();
  _inflight = InFlight::None;
  if (_q) spill(0);
  else { _lost += _count; _count = 0; _first = 0; }
}
