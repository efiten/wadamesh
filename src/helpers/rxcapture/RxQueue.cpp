// SPDX-License-Identifier: GPL-3.0-or-later
#include "RxQueue.h"

#include <string.h>

namespace {

// Checks, one character at a time, that a line is a kind character followed by
// exactly one JSON object. Braces inside strings are not structure.
struct RecordCheck {
  size_t pos = 0;
  int depth = 0;
  bool in_str = false, esc = false, closed = false, bad = false;

  void feed(char c) {
    const size_t i = pos++;
    if (bad) return;
    if (i == 0) { bad = !(c == 'p' || c == 'r'); return; }
    if (i == 1) { if (c == '{') depth = 1; else bad = true; return; }
    if (closed) { bad = true; return; }
    if (in_str) {
      if (esc) esc = false;
      else if (c == '\\') esc = true;
      else if (c == '"') in_str = false;
      return;
    }
    if (c == '"') in_str = true;
    else if (c == '{') depth++;
    else if (c == '}') { if (--depth == 0) closed = true; }
  }
  bool valid() const { return !bad && closed; }
};

}  // namespace

bool RxQueue::scanLine(size_t off, size_t* len, bool* valid, char* out, size_t cap, bool* copied) {
  RecordCheck check;
  char chunk[256];
  size_t n_line = 0;
  bool fits = out != nullptr;
  for (;;) {
    const size_t got = _store.readAt(off + n_line, chunk, sizeof chunk);
    if (got == 0) return false;
    for (size_t i = 0; i < got; ++i) {
      const char c = chunk[i];
      if (c == '\n') {
        *len = n_line;
        *valid = check.valid() && n_line <= kMaxLine;
        if (copied) *copied = *valid && fits;
        if (out && fits) out[n_line] = 0;
        return true;
      }
      check.feed(c);
      if (fits) {
        if (n_line + 1 < cap) out[n_line] = c;
        else fits = false;
      }
      n_line++;
    }
  }
}

// A file that does not end in a newline ends in a torn line. Close it, so the
// next record starts on a line of its own; the torn one fails the record check.
void RxQueue::terminateTail() {
  if (_store.append("\n", 1)) { _size += 1; _tail_dirty = false; }
  else _tail_dirty = true;
}

void RxQueue::empty() {
  _store.reset();
  _size = 0;
  _head = 0;
  _peek_end = 0;
  _pending = 0;
  _acks = 0;
  _tail_dirty = false;
}

void RxQueue::begin() {
  _size = _store.size();
  _peek_end = 0;
  _pending = 0;
  _acks = 0;
  _tail_dirty = false;
  size_t ack = 0;
  if (!_store.loadAck(&ack)) ack = 0;
  if (ack > _size) { empty(); return; }   // the offset belongs to another file: nothing here can be trusted
  _head = ack;
  if (_size > 0) {
    char last;
    if (_store.readAt(_size - 1, &last, 1) == 1 && last != '\n') terminateTail();
  }
  size_t off = _head, len;
  bool valid;
  while (scanLine(off, &len, &valid, nullptr, 0, nullptr)) {
    if (valid) _pending++;
    off += len + 1;
  }
}

bool RxQueue::push(const char* line) {
  const size_t n = strlen(line);
  RecordCheck check;
  for (size_t i = 0; i < n; ++i) check.feed(line[i]);
  if (!check.valid() || n > kMaxLine) { _dropped++; return false; }

  if (_tail_dirty || _store.size() != _size) {   // an earlier write was cut short
    _size = _store.size();
    char last;
    if (_size > 0 && _store.readAt(_size - 1, &last, 1) == 1 && last != '\n') terminateTail();
    else _tail_dirty = false;
    if (_tail_dirty) { _dropped++; return false; }
  }

  if (_size + n + 1 > _cap) { _dropped++; return false; }
  char buf[kMaxLine + 1];
  memcpy(buf, line, n);
  buf[n] = '\n';
  if (!_store.append(buf, n + 1)) { _tail_dirty = true; _dropped++; return false; }
  _size += n + 1;
  _pending++;
  return true;
}

bool RxQueue::peek(char* out, size_t cap) {
  for (;;) {
    size_t len;
    bool valid, copied;
    if (!scanLine(_head, &len, &valid, out, cap, &copied)) {
      // Nothing sendable is left. Lines stepped over may still occupy the file.
      if (_pending == 0 && _head > 0 && !_tail_dirty && _store.size() == _size) empty();
      return false;
    }
    if (copied) {
      _peek_end = _head + len + 1;
      return true;
    }
    // Torn or foreign lines are stepped over silently; a real record that does
    // not fit the caller's buffer is lost and counted.
    if (valid) { _dropped++; if (_pending) _pending--; }
    else if (len > kMaxLine) _dropped++;
    _head += len + 1;
    _peek_end = 0;
  }
}

void RxQueue::ack() {
  if (_peek_end == 0) return;
  _head = _peek_end;
  _peek_end = 0;
  if (_pending) _pending--;
  if (_head >= _size && !_tail_dirty && _store.size() == _size) { empty(); return; }
  if (++_acks >= kAckSaveEvery) {
    _store.saveAck(_head);
    _acks = 0;
  }
}
