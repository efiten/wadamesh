// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Decides where an RX-capture record goes: straight to the broker, into a small
// memory buffer while an earlier one is in flight, or into the file queue.
//
// A line is a kind character plus the JSON, as RxQueue stores it. Records are
// published one at a time. With the link up they wait in memory, so a burst of
// receptions does not write to the file. They go to the file when the link is
// down, when a publish fails, when memory is full, or when older records are
// already waiting there, so the file is always sent before memory.
//
// Order is kept except in one case: memory fills while a record is in flight and
// that record then fails. It is queued behind the ones that were spilled.
//
// No Arduino dependency: host-tested by test/rxcapture/run.sh.

#include <stddef.h>
#include <stdint.h>

#include "RxQueue.h"

struct RxLinkPort {
  virtual bool up() = 0;                        // a broker session exists
  virtual bool canPublish() = 0;                // up and nothing in flight
  virtual bool publish(const char* line) = 0;   // false = not accepted, nothing in flight
  virtual bool takeAck() = 0;                   // true once: the publish in flight was acknowledged
  virtual bool takeFailure() = 0;               // true once: it timed out or the link dropped under it
  virtual ~RxLinkPort() {}
};

class RxPump {
public:
  // mem holds slots lines of line_cap bytes; scratch is one line of line_cap
  // bytes for a record read back from the file.
  RxPump(RxLinkPort& link, char* mem, uint8_t slots, size_t line_cap, char* scratch)
      : _link(link), _mem(mem), _slots(slots), _cap(line_cap), _scratch(scratch) {}

  void setQueue(RxQueue* q) { _q = q; }   // nullptr = no file available
  void submit(const char* line);          // a new record
  void service();                         // every pass of the main loop
  void shutdown();                        // the session was stopped: unsent records go to the file

  uint8_t buffered() const { return _count; }
  uint32_t lost() const { return _lost; }   // records dropped here; the queue counts its own

private:
  enum class InFlight : uint8_t { None, Memory, File };

  char* slot(uint8_t i) { return _mem + (size_t)((_first + i) % _slots) * _cap; }
  void append(const char* line);
  void settle();                 // collect the ack or failure of the record in flight
  void spill(uint8_t keep);      // move all but the first `keep` buffered lines to the file

  RxLinkPort& _link;
  char* _mem;
  uint8_t _slots;
  size_t _cap;
  char* _scratch;
  RxQueue* _q = nullptr;
  uint8_t _first = 0, _count = 0;
  InFlight _inflight = InFlight::None;
  uint32_t _lost = 0;
};
