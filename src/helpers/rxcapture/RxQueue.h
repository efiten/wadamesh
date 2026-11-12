// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// File-backed queue for RX-capture records that could not be published yet.
//
// One record per line: a kind character ('p' = packets, 'r' = rf), the JSON
// object, a newline. Records are only ever appended. An ack offset (the byte
// offset of the first line not yet acknowledged by the broker) is saved after
// every kAckSaveEvery acks, so a crash re-sends at most that many records minus
// one, and none is skipped. When the last line is acked the file is emptied.
//
// A line is accepted and returned only if it is a complete record. That is how a
// line torn by a power cut, or by a write the filesystem cut short, is stepped
// over instead of being published.
//
// No Arduino dependency: host-tested by test/rxcapture/run.sh.

#include <stddef.h>
#include <stdint.h>

struct RxStore {
  virtual size_t size() = 0;                                // bytes in the data file
  virtual bool append(const char* data, size_t n) = 0;      // false = the write failed (it may be partial)
  virtual size_t readAt(size_t off, char* buf, size_t cap) = 0;
  virtual bool loadAck(size_t* off) = 0;                    // false = no saved offset
  virtual bool saveAck(size_t off) = 0;
  virtual bool reset() = 0;                                 // empty the data file, ack offset 0
  virtual ~RxStore() {}
};

class RxQueue {
public:
  static constexpr size_t kMaxLine = 511;        // kind + JSON, without the newline
  static constexpr uint32_t kAckSaveEvery = 10;  // coredrive-rx drain.js COMMIT_EVERY

  RxQueue(RxStore& store, size_t cap_bytes) : _store(store), _cap(cap_bytes) {}

  void begin();

  // line = kind + JSON, no newline. false = not stored: the queue is full, the
  // line is not a complete record, or the write failed. Each counts in dropped().
  bool push(const char* line);

  // The oldest unacked record into out, NUL-terminated, without the newline.
  // false = nothing to send. Repeated calls return the same record until ack().
  bool peek(char* out, size_t cap);

  // The record last returned by peek() was acknowledged by the broker.
  void ack();

  uint32_t pending() const { return _pending; }
  uint32_t dropped() const { return _dropped; }

private:
  // Reads the line starting at off. false = no newline before the end of the file.
  // *len is the line length without the newline; *valid says whether it is a
  // complete record of at most kMaxLine bytes. With out set, a valid line that
  // fits is copied there and *copied is set.
  bool scanLine(size_t off, size_t* len, bool* valid, char* out, size_t cap, bool* copied);
  void terminateTail();
  void empty();

  RxStore& _store;
  size_t _cap;
  size_t _size = 0;        // bytes this queue believes are in the file
  size_t _head = 0;        // offset of the first unacked line
  size_t _peek_end = 0;    // offset after the line peek() returned; 0 = none
  uint32_t _pending = 0;
  uint32_t _dropped = 0;
  uint32_t _acks = 0;      // acks since the offset was last saved
  bool _tail_dirty = false;
};
