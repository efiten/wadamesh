// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(ESP32) && defined(RX_CAPTURE)

#include "RxCapture.h"

#include <FS.h>
#include <SD.h>
#include <SPIFFS.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <target.h>

#include "../../MyMesh.h"
#include "../rxcapture/RxGate.h"
#include "../rxcapture/RxQueue.h"
#include "../rxcapture/RxRecord.h"
#include "DriveMode.h"
#include "WdtHeavyGuard.h"

extern PagerGps gps;         // variants/lilygo_tlora_pager/target.cpp
bool uiSdMounted();           // UITask.cpp

RxCapture rxCapture;

namespace {

const char* const kDir      = "/rxcap";
const char* const kDataPath = "/rxcap/queue.jsonl";
const char* const kAckPath  = "/rxcap/queue.ack";
const char* const kAckTmp   = "/rxcap/queue.ack.tmp";
const size_t kCapSd       = 64u * 1024u * 1024u;
const size_t kCapInternal = 1024u * 1024u;

// The queue file on a filesystem the firmware already mounted. One handle, opened
// for append and read: every write lands at the end, reads seek first.
class FsRxStore : public RxStore {
public:
  bool open(fs::FS* fs) {
    _fs = fs;
    if (!_fs->exists(kDir)) _fs->mkdir(kDir);
    return reopen();
  }
  void close() {
    if (_f) _f.close();
    _fs = nullptr;
  }
  bool isOpen() { return _fs && _f; }

  size_t size() override { return _size; }
  bool append(const char* data, size_t n) override {
    if (!_f) return false;
    WdtHeavyGuard guard;
    const size_t w = _f.write((const uint8_t*)data, n);
    _f.flush();
    _size += w;
    return w == n;
  }
  size_t readAt(size_t off, char* buf, size_t cap) override {
    if (!_f || !_f.seek(off)) return 0;
    return _f.read((uint8_t*)buf, cap);
  }
  bool loadAck(size_t* off) override {
    File a = _fs->open(kAckPath, "r");
    if (!a) return false;
    char txt[16] = {};
    const size_t n = a.read((uint8_t*)txt, sizeof(txt) - 1);
    a.close();
    if (n == 0) return false;
    char* end = nullptr;
    const unsigned long v = strtoul(txt, &end, 10);
    if (end == txt) return false;
    *off = (size_t)v;
    return true;
  }
  bool saveAck(size_t off) override {
    WdtHeavyGuard guard;
    File a = _fs->open(kAckTmp, "w");
    if (!a) return false;
    const size_t n = a.print((unsigned long)off);
    a.close();
    if (n == 0) return false;
    _fs->remove(kAckPath);   // FAT does not rename onto an existing file
    return _fs->rename(kAckTmp, kAckPath);
  }
  bool reset() override {
    WdtHeavyGuard guard;
    if (_f) _f.close();
    _fs->remove(kDataPath);
    _fs->remove(kAckPath);
    return reopen();
  }

private:
  bool reopen() {
    _f = _fs->open(kDataPath, "a+");
    _size = _f ? _f.size() : 0;
    return (bool)_f;
  }
  fs::FS* _fs = nullptr;
  File _f;
  size_t _size = 0;
};

struct RingEntry {
  uint32_t ms;
  float snr, rssi;
  uint16_t len;
  uint8_t raw[256];
};
const uint8_t kRingSize = 32;

RingEntry* s_ring = nullptr;        // PSRAM
volatile uint8_t s_ring_head = 0;   // next slot to write
volatile uint8_t s_ring_tail = 0;   // next slot to read
volatile uint32_t s_ring_dropped = 0;

const size_t kLineCap = RxQueue::kMaxLine + 1;
char* s_line = nullptr;   // PSRAM: the record being built, or read from the queue
char* s_live = nullptr;   // PSRAM: the record published live, kept until its PUBACK

FsRxStore s_store;
RxQueue* s_queue = nullptr;
RxGate s_gate;
bool s_on_sd = false;
bool s_was_active = false;

enum class InFlight : uint8_t { None, Live, Queued };
InFlight s_inflight = InFlight::None;

bool s_have_fix = false;
RxFix s_fix{};
uint32_t s_captured = 0;
uint32_t s_store_dropped = 0;   // records lost because no queue file could be opened
float s_last_snr = 0;

void openQueue() {
  s_on_sd = uiSdMounted();
  fs::FS* fs = s_on_sd ? (fs::FS*)&SD : (fs::FS*)&SPIFFS;
  if (!s_store.open(fs)) {
    Serial.printf("rx: cannot open %s on %s\n", kDataPath, s_on_sd ? "SD" : "internal flash");
    s_store.close();
    return;
  }
  s_queue = new RxQueue(s_store, s_on_sd ? kCapSd : kCapInternal);
  s_queue->begin();
}

void closeQueue() {
  delete s_queue;
  s_queue = nullptr;
  s_store.close();
}

bool queuePush(const char* line) {
  if (!s_queue) { s_store_dropped++; return false; }
  return s_queue->push(line);
}

bool publishLine(const char* line) {
  char topic[112];
  if (!rxTopic(topic, sizeof(topic), rxTransport.pubkey(), line[0])) return false;
  return rxTransport.publish(topic, line + 1);
}

// line = kind + JSON. Live when the link is free and nothing older is waiting,
// otherwise into the queue, so records keep their order.
void sendRecord(const char* line) {
  const bool queue_empty = !s_queue || s_queue->pending() == 0;
  if (s_inflight == InFlight::None && queue_empty && rxTransport.canPublish()) {
    strncpy(s_live, line, kLineCap - 1);
    s_live[kLineCap - 1] = 0;
    if (publishLine(s_live)) { s_inflight = InFlight::Live; return; }
  }
  queuePush(line);
}

void feedGate(uint32_t now) {
  s_gate.setClockValid(rtc_clock.timeIsCurrent());
  if (gps.isValid()) {
    s_fix.lat = gps.getLatitude() / 1000000.0;
    s_fix.lon = gps.getLongitude() / 1000000.0;
    s_fix.acc_m = gps.getHdopTenths() * 0.5f;   // HDOP x 5 m
    s_have_fix = true;
    s_gate.onFix(s_fix, now);
  } else {
    s_have_fix = false;
    s_gate.onNoFix();
  }
}

void drainRing(uint32_t now) {
  const char* name = the_mesh.getNodeName();
  while (s_ring_tail != s_ring_head) {
    const RingEntry& e = s_ring[s_ring_tail];
    s_gate.onTrafficHeard(e.ms);
    s_last_snr = e.snr;
    if (s_gate.captureAllowed()) {
      RxPacket p{};
      p.epoch = rtc_clock.getCurrentTime() - (uint32_t)(now - e.ms) / 1000u;
      p.snr = e.snr;
      p.rssi = (int)e.rssi;
      p.raw = e.raw;
      p.len = e.len;
      p.fix = s_fix;
      s_line[0] = 'p';
      if (rxBuildPacketJson(s_line + 1, kLineCap - 1, rxTransport.pubkey(), name, p)) {
        s_captured++;
        sendRecord(s_line);
      }
    }
    s_ring_tail = (uint8_t)((s_ring_tail + 1) % kRingSize);
  }
}

void sampleRf() {
  RxRfSample s{};
  the_mesh.fillRfSample(s);
  s.epoch = rtc_clock.getCurrentTime();
  s.fix = s_fix;
  s.stationary = s_gate.stationary();
  s_line[0] = 'r';
  if (rxBuildRfJson(s_line + 1, kLineCap - 1, rxTransport.pubkey(), the_mesh.getNodeName(), s)) sendRecord(s_line);
}

// PUBACKs, failures, and the next queued record.
void pumpTransport() {
  if (rxTransport.takeAck()) {
    if (s_inflight == InFlight::Queued && s_queue) s_queue->ack();
    s_inflight = InFlight::None;
  }
  if (rxTransport.takeFailure()) {
    if (s_inflight == InFlight::Live) queuePush(s_live);   // a queued record simply stays queued
    s_inflight = InFlight::None;
  }
  if (s_inflight == InFlight::None && s_queue && s_queue->pending() > 0 && rxTransport.canPublish()) {
    if (s_queue->peek(s_line, kLineCap) && publishLine(s_line)) s_inflight = InFlight::Queued;
  }
}

}  // namespace

void RxCapture::begin() {
  s_ring = (RingEntry*)heap_caps_calloc(kRingSize, sizeof(RingEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  s_line = (char*)heap_caps_calloc(1, kLineCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  s_live = (char*)heap_caps_calloc(1, kLineCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void RxCapture::onRxRaw(float snr, float rssi, const uint8_t* raw, int len) {
  if (!DriveMode::active() || !s_ring || len <= 0 || len > 256) return;
  const uint8_t next = (uint8_t)((s_ring_head + 1) % kRingSize);
  if (next == s_ring_tail) { s_ring_dropped++; return; }
  RingEntry& e = s_ring[s_ring_head];
  e.ms = millis();
  e.snr = snr;
  e.rssi = rssi;
  e.len = (uint16_t)len;
  memcpy(e.raw, raw, (size_t)len);
  s_ring_head = next;
}

void RxCapture::loop() {
  if (!s_ring || !s_line || !s_live) return;
  const bool active = DriveMode::active();
  if (active && !s_was_active) {
    s_gate = RxGate();
    s_ring_tail = s_ring_head;
    openQueue();
  }
  if (!active) {
    if (s_was_active) {
      pumpTransport();   // drive off stopped the session: a live record in flight goes to the queue
      closeQueue();
      s_inflight = InFlight::None;
      s_was_active = false;
    }
    return;
  }
  s_was_active = true;

  const uint32_t now = millis();
  feedGate(now);
  drainRing(now);
  if (s_gate.rfSampleDue(now)) sampleRf();
  if (s_gate.discoverDue(now)) the_mesh.uiStartDiscoverScan(0);
  pumpTransport();
}

void RxCapture::getStatus(RxCaptureStatus& out) const {
  out.drive = DriveMode::active();
  out.fix = s_have_fix;
  out.acc_m = s_have_fix ? s_fix.acc_m : 0;
  out.stationary = s_gate.stationary();
  out.link = rxTransport.link();
  out.captured = s_captured;
  out.sent = rxTransport.sent();
  out.pending = s_queue ? s_queue->pending() : 0;
  out.dropped = s_ring_dropped + s_store_dropped + (s_queue ? s_queue->dropped() : 0);
  out.last_snr = s_last_snr;
  out.sd = s_on_sd;
}

void RxCapture::formatStatus(char* out, size_t cap) const {
  RxCaptureStatus st;
  getStatus(st);
  snprintf(out, cap, "capture: %s, fix %s acc %.1f m, %s, clock %s, captured %lu, pending %lu, dropped %lu, queue on %s",
           st.drive ? "on" : "off", st.fix ? "yes" : "no", (double)st.acc_m,
           st.stationary ? "stationary" : "moving", rtc_clock.timeIsCurrent() ? "ok" : "not set",
           (unsigned long)st.captured, (unsigned long)st.pending, (unsigned long)st.dropped,
           !s_queue ? "(closed)" : st.sd ? "SD" : "internal flash");
}

#endif
