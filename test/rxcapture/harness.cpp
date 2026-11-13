// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for src/helpers/rxcapture. Run through test/rxcapture/run.sh.
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

#include "RxRecord.h"
#include "RxGate.h"
#include "RxQueue.h"
#include "RxPump.h"

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond); g_failed++; } } while (0)
#define CHECK_STR(got, want) do { if (strcmp((got), (want)) != 0) { \
  printf("FAIL %s:%d\n  got:  %s\n  want: %s\n", __func__, __LINE__, (got), (want)); g_failed++; } } while (0)

// ---- RxRecord -------------------------------------------------------------

static RxPacket samplePacket(const uint8_t* raw, int len) {
  RxPacket p{};
  p.epoch = 1794518777u; p.snr = -7.25f; p.rssi = -101; p.raw = raw; p.len = len;
  p.fix = {51.12345, 4.54321, 12.5f};
  return p;
}

static void test_packet_json() {
  const uint8_t raw[] = {0x11, 0x00, 0xAB};
  char buf[512];
  size_t n = rxBuildPacketJson(buf, sizeof buf, "ab12", "ON8AR pager", samplePacket(raw, 3));
  CHECK_STR(buf,
    "{\"origin_id\":\"ab12\",\"origin\":\"ON8AR pager\",\"timestamp\":\"2026-11-12T21:26:17Z\","
    "\"type\":\"PACKET\",\"direction\":\"rx\",\"raw\":\"1100ab\",\"SNR\":-7.25,\"RSSI\":-101,"
    "\"gps\":{\"lat\":51.12345,\"lon\":4.54321,\"acc_m\":12.5}}");
  CHECK(n == strlen(buf));
}

static void test_packet_json_escapes_name() {
  const uint8_t raw[] = {0x01};
  char buf[512];
  rxBuildPacketJson(buf, sizeof buf, "ab12", "a\"b\\c", samplePacket(raw, 1));
  CHECK(strstr(buf, "\"origin\":\"a\\\"b\\\\c\",") != nullptr);
}

static void test_packet_json_control_char_in_name() {
  const uint8_t raw[] = {0x01};
  char buf[512];
  rxBuildPacketJson(buf, sizeof buf, "ab12", "a\nb", samplePacket(raw, 1));
  CHECK(strstr(buf, "\"origin\":\"a\\u000ab\",") != nullptr);
  CHECK(strchr(buf, '\n') == nullptr);   // a raw newline would split the queue line
}

static void test_packet_json_no_name() {
  const uint8_t raw[] = {0x01};
  char buf[512];
  rxBuildPacketJson(buf, sizeof buf, "ab12", "", samplePacket(raw, 1));
  CHECK(strstr(buf, "\"origin\"") == nullptr);
  CHECK(strncmp(buf, "{\"origin_id\":\"ab12\",\"timestamp\":", 32) == 0);
}

static void test_packet_json_too_small() {
  const uint8_t raw[] = {0x11, 0x00, 0xAB};
  char buf[32];
  CHECK(rxBuildPacketJson(buf, sizeof buf, "ab12", "x", samplePacket(raw, 3)) == 0);
}

static void test_packet_json_exact_fit() {
  const uint8_t raw[] = {0x11, 0x00, 0xAB};
  char big[512];
  size_t n = rxBuildPacketJson(big, sizeof big, "ab12", "x", samplePacket(raw, 3));
  CHECK(n > 0);
  std::string fit(n + 1, '#');
  CHECK(rxBuildPacketJson(&fit[0], n + 1, "ab12", "x", samplePacket(raw, 3)) == n);   // n chars + NUL
  std::string tight(n, '#');
  CHECK(rxBuildPacketJson(&tight[0], n, "ab12", "x", samplePacket(raw, 3)) == 0);
}

static void test_rf_json() {
  RxRfSample s{};
  s.epoch = 1794518777u; s.fix = {51.12345, 4.54321, 12.5f}; s.stationary = true;
  s.uptime_secs = 3600; s.battery_mv = 4012; s.queue_len = 2; s.errors = 5;
  s.noise_floor = -112; s.last_rssi = -98; s.last_snr = 6.5f;
  s.tx_air_secs = 11; s.rx_air_secs = 222; s.recv = 1000; s.sent = 40;
  s.flood_rx = 700; s.direct_rx = 300; s.flood_tx = 30; s.direct_tx = 10; s.recv_errors = 7;
  char buf[768];
  size_t n = rxBuildRfJson(buf, sizeof buf, "ab12", "ON8AR pager", s);
  CHECK_STR(buf,
    "{\"origin_id\":\"ab12\",\"origin\":\"ON8AR pager\",\"timestamp\":\"2026-11-12T21:26:17Z\","
    "\"type\":\"RF_SAMPLE\",\"gps\":{\"lat\":51.12345,\"lon\":4.54321,\"acc_m\":12.5},"
    "\"stationary\":true,\"uptime_secs\":3600,\"battery_mv\":4012,\"queue_len\":2,\"errors\":5,"
    "\"noise_floor\":-112,\"last_rssi\":-98,\"last_snr\":6.50,"
    "\"tx_air_secs\":11,\"rx_air_secs\":222,\"recv\":1000,\"sent\":40,"
    "\"flood_rx\":700,\"direct_rx\":300,\"flood_tx\":30,\"direct_tx\":10,\"recv_errors\":7}");
  CHECK(n == strlen(buf));
  s.stationary = false;
  rxBuildRfJson(buf, sizeof buf, "ab12", "ON8AR pager", s);
  CHECK(strstr(buf, "\"stationary\":false,") != nullptr);
}

static void test_topic() {
  char t[128];
  CHECK(rxTopic(t, sizeof t, "ab12", 'p') == strlen("meshcore/client/ab12/packets"));
  CHECK_STR(t, "meshcore/client/ab12/packets");
  rxTopic(t, sizeof t, "ab12", 'r');
  CHECK_STR(t, "meshcore/client/ab12/rf");
  CHECK(rxTopic(t, sizeof t, "ab12", 'x') == 0);
  CHECK(rxTopic(t, 10, "ab12", 'p') == 0);
}

static void test_iso() {
  char out[21];
  rxIso8601(out, 1794518777u); CHECK_STR(out, "2026-11-12T21:26:17Z");
  rxIso8601(out, 951782400u);  CHECK_STR(out, "2000-02-29T00:00:00Z");
  rxIso8601(out, 0u);          CHECK_STR(out, "1970-01-01T00:00:00Z");
  rxIso8601(out, 4102444799u); CHECK_STR(out, "2099-12-31T23:59:59Z");
}

// ---- RxGate ---------------------------------------------------------------

static const double kMPerDegLat = 111194.93;   // 2*pi*6371000/360
static RxFix fixAt(double metres_north) { return {51.0 + metres_north / kMPerDegLat, 4.5, 8.0f}; }

static void test_distance() {
  const double d = rxDistanceM(fixAt(0), fixAt(100));
  CHECK(d > 99.9 && d < 100.1);
  CHECK(rxDistanceM(fixAt(0), fixAt(0)) == 0.0);
}

static void test_no_fix_no_capture() {
  RxGate g;
  g.setClockValid(true);
  CHECK(!g.captureAllowed());
  CHECK(!g.rfSampleDue(0));
  CHECK(!g.discoverDue(0));
}

static void test_clock_invalid_no_capture() {
  RxGate g;
  g.onFix(fixAt(0), 0);
  CHECK(!g.captureAllowed());          // the clock starts invalid
  CHECK(!g.rfSampleDue(0));
  g.setClockValid(true);
  CHECK(g.captureAllowed());
  g.setClockValid(false);
  CHECK(!g.captureAllowed());
}

static void test_fix_lost_stops_capture() {
  RxGate g;
  g.setClockValid(true);
  g.onFix(fixAt(0), 0);
  CHECK(g.captureAllowed());
  g.onNoFix();
  CHECK(!g.captureAllowed());
  CHECK(!g.rfSampleDue(1000));
  g.onFix(fixAt(0), 2000);
  CHECK(g.captureAllowed());
}

static void test_moving_captures() {
  RxGate g;
  g.setClockValid(true);
  for (int i = 0; i < 60; ++i) {       // 200 m every 10 s for 10 minutes
    g.onFix(fixAt(200.0 * i), 10000u * i);
    CHECK(g.captureAllowed());
    CHECK(!g.stationary());
  }
}

static void test_stationary_after_dwell() {
  RxGate g;
  g.setClockValid(true);
  g.onFix(fixAt(0), 0);
  g.onFix(fixAt(10), 299999);          // inside the 75 m radius, one ms short of the dwell
  CHECK(!g.stationary());
  g.onFix(fixAt(0), 300000);
  CHECK(g.stationary());
  CHECK(!g.captureAllowed());
}

static void test_stationary_ends_on_move() {
  RxGate g;
  g.setClockValid(true);
  g.onFix(fixAt(0), 0);
  g.onFix(fixAt(0), 300000);
  CHECK(g.stationary());
  g.onFix(fixAt(75), 300500);          // exactly on the radius is still inside
  CHECK(g.stationary());
  g.onFix(fixAt(76), 301000);
  CHECK(!g.stationary());
  CHECK(g.captureAllowed());
}

static void test_rf_first_sample_at_once() {
  RxGate g;
  g.setClockValid(true);
  g.onFix(fixAt(0), 5000);
  CHECK(g.rfSampleDue(5000));
  CHECK(!g.rfSampleDue(5000));         // true once per due sample
}

static void test_rf_due_by_distance() {
  RxGate g;
  g.setClockValid(true);
  g.onFix(fixAt(0), 0);
  CHECK(g.rfSampleDue(0));
  g.onFix(fixAt(99), 2000);
  CHECK(!g.rfSampleDue(2000));
  g.onFix(fixAt(101), 3000);
  CHECK(g.rfSampleDue(3000));
  CHECK(!g.rfSampleDue(3001));
}

static void test_rf_due_by_time() {
  RxGate g;
  g.setClockValid(true);
  g.onFix(fixAt(0), 0);
  CHECK(g.rfSampleDue(0));
  g.onFix(fixAt(10), 14999);
  CHECK(!g.rfSampleDue(14999));
  CHECK(g.rfSampleDue(15000));
}

static void test_rf_due_parked() {
  RxGate g;
  g.setClockValid(true);
  g.onFix(fixAt(0), 0);
  CHECK(g.rfSampleDue(0));
  g.onFix(fixAt(0), 300000);           // parked from here on
  CHECK(g.stationary());
  CHECK(g.rfSampleDue(300000));
  g.onFix(fixAt(0), 400000);
  CHECK(!g.rfSampleDue(400000));       // 15 s rule no longer applies
  g.onFix(fixAt(0), 599999);
  CHECK(!g.rfSampleDue(599999));
  CHECK(g.rfSampleDue(600000));
}

static void test_discover_30s() {
  RxGate g;
  g.setClockValid(true);
  g.onFix(fixAt(0), 0);
  CHECK(g.discoverDue(0));
  CHECK(!g.discoverDue(1));
  g.onFix(fixAt(300), 29999);
  CHECK(!g.discoverDue(29999));
  CHECK(g.discoverDue(30000));
  CHECK(!g.discoverDue(30001));
}

static void test_discover_backoff() {
  RxGate g;
  g.setClockValid(true);
  g.onFix(fixAt(0), 0);
  CHECK(g.discoverDue(0));
  g.onTrafficHeard(29000);
  g.onFix(fixAt(300), 30000);
  CHECK(!g.discoverDue(30000));
  CHECK(!g.discoverDue(43999));
  CHECK(g.discoverDue(44000));
}

static void test_discover_backoff_before_first() {
  RxGate g;
  g.setClockValid(true);
  g.onTrafficHeard(1000);
  g.onFix(fixAt(0), 2000);
  CHECK(!g.discoverDue(2000));
  CHECK(g.discoverDue(16000));
}

static void test_discover_not_when_stationary() {
  RxGate g;
  g.setClockValid(true);
  g.onFix(fixAt(0), 0);
  CHECK(g.discoverDue(0));
  g.onFix(fixAt(0), 300000);
  CHECK(g.stationary());
  CHECK(!g.discoverDue(300000));
  CHECK(!g.discoverDue(900000));
}

static void test_millis_wrap() {
  const uint32_t t0 = 0xFFFFF000u;     // 4096 ms before the counter wraps
  RxGate g;
  g.setClockValid(true);
  g.onFix(fixAt(0), t0);
  CHECK(g.discoverDue(t0));
  CHECK(g.rfSampleDue(t0));
  g.onFix(fixAt(10), t0 + 14999u);     // already wrapped
  CHECK(!g.rfSampleDue(t0 + 14999u));
  CHECK(g.rfSampleDue(t0 + 15000u));
  CHECK(!g.discoverDue(t0 + 29999u));
  CHECK(g.discoverDue(t0 + 30000u));
  g.onFix(fixAt(10), t0 + 299999u);
  CHECK(!g.stationary());
  g.onFix(fixAt(10), t0 + 300000u);
  CHECK(g.stationary());
}

// ---- RxQueue --------------------------------------------------------------

struct FakeStore : RxStore {
  std::string data;
  bool has_ack = false;
  size_t ack = 0;
  bool fail_append = false;      // append writes nothing and reports failure
  size_t partial_append = 0;     // append writes this many bytes, then reports failure
  int resets = 0, ack_saves = 0;

  size_t size() override { return data.size(); }
  bool append(const char* d, size_t n) override {
    if (fail_append) return false;
    if (partial_append) { data.append(d, partial_append < n ? partial_append : n); return false; }
    data.append(d, n);
    return true;
  }
  size_t readAt(size_t off, char* buf, size_t cap) override {
    if (off >= data.size()) return 0;
    size_t n = data.size() - off;
    if (n > cap) n = cap;
    memcpy(buf, data.data() + off, n);
    return n;
  }
  bool loadAck(size_t* off) override { if (!has_ack) return false; *off = ack; return true; }
  bool saveAck(size_t off) override { has_ack = true; ack = off; ack_saves++; return true; }
  bool reset() override { data.clear(); has_ack = true; ack = 0; resets++; return true; }
};

static std::string rec(int i) { return "p{\"n\":" + std::to_string(i) + "}"; }

static bool peekIs(RxQueue& q, const std::string& want) {
  char line[512];
  if (!q.peek(line, sizeof line)) return false;
  return want == line;
}

static void test_queue_fifo() {
  FakeStore st;
  RxQueue q(st, 4096);
  q.begin();
  char line[512];
  CHECK(!q.peek(line, sizeof line));
  CHECK(q.push(rec(1).c_str()) && q.push(rec(2).c_str()) && q.push("r{\"n\":3}"));
  CHECK(q.pending() == 3);
  CHECK(peekIs(q, rec(1))); q.ack();
  CHECK(peekIs(q, rec(2))); q.ack();
  CHECK(peekIs(q, "r{\"n\":3}")); q.ack();
  CHECK(!q.peek(line, sizeof line));
  CHECK(q.pending() == 0);
  CHECK(q.dropped() == 0);
}

static void test_queue_peek_is_stable() {
  FakeStore st;
  RxQueue q(st, 4096);
  q.begin();
  q.push(rec(1).c_str()); q.push(rec(2).c_str());
  CHECK(peekIs(q, rec(1)));
  CHECK(peekIs(q, rec(1)));
  CHECK(q.pending() == 2);
}

static void test_queue_ack_without_peek_is_ignored() {
  FakeStore st;
  RxQueue q(st, 4096);
  q.begin();
  q.push(rec(1).c_str());
  q.ack();
  CHECK(q.pending() == 1);
  CHECK(peekIs(q, rec(1)));
}

static void test_queue_cap_drops_new() {
  FakeStore st;
  RxQueue q(st, 64);
  q.begin();
  int accepted = 0;
  while (q.push(rec(accepted + 1).c_str())) accepted++;
  CHECK(accepted > 0);
  CHECK(st.data.size() <= 64);
  CHECK(q.dropped() == 1);
  CHECK(q.pending() == (uint32_t)accepted);
  for (int i = 1; i <= accepted; ++i) { CHECK(peekIs(q, rec(i))); q.ack(); }
  CHECK(q.pending() == 0);
}

static void test_queue_reset_when_empty() {
  FakeStore st;
  RxQueue q(st, 4096);
  q.begin();
  q.push(rec(1).c_str()); q.push(rec(2).c_str());
  CHECK(peekIs(q, rec(1))); q.ack();
  CHECK(st.data.size() > 0);
  CHECK(peekIs(q, rec(2))); q.ack();
  CHECK(st.data.size() == 0);
  CHECK(st.resets == 1);
  q.push(rec(3).c_str());
  CHECK(st.data == rec(3) + "\n");
  CHECK(peekIs(q, rec(3)));
}

static void test_queue_restart_resumes() {
  FakeStore st;
  {
    RxQueue q(st, 65536);
    q.begin();
    for (int i = 1; i <= 25; ++i) CHECK(q.push(rec(i).c_str()));
    for (int i = 1; i <= 12; ++i) { CHECK(peekIs(q, rec(i))); q.ack(); }
    CHECK(st.ack_saves == 1);          // saved once, after the 10th ack
  }
  RxQueue q2(st, 65536);
  q2.begin();
  CHECK(q2.pending() == 15);           // 11 and 12 are sent again, none is skipped
  CHECK(peekIs(q2, rec(11)));
}

static void test_queue_torn_last_line() {
  FakeStore st;
  st.data = rec(1) + "\n" + "p{\"origin";   // power cut in the middle of an append
  RxQueue q(st, 4096);
  q.begin();
  CHECK(q.pending() == 1);
  CHECK(q.push(rec(2).c_str()));
  CHECK(q.pending() == 2);
  CHECK(peekIs(q, rec(1))); q.ack();
  CHECK(peekIs(q, rec(2))); q.ack();   // the torn line is stepped over
  char line[512];
  CHECK(!q.peek(line, sizeof line));
  CHECK(q.pending() == 0);
}

static void test_queue_torn_line_that_ends_in_brace() {
  FakeStore st;
  st.data = "p{\"gps\":{\"lat\":1.0}";       // the inner object closed, the outer one did not
  RxQueue q(st, 4096);
  q.begin();
  CHECK(q.pending() == 0);
  q.push(rec(1).c_str());
  CHECK(peekIs(q, rec(1)));
}

static void test_queue_brace_inside_string_is_not_structure() {
  FakeStore st;
  RxQueue q(st, 4096);
  q.begin();
  const char* tricky = "p{\"origin\":\"a}b{\\\"c\",\"n\":1}";
  CHECK(q.push(tricky));
  CHECK(q.pending() == 1);
  CHECK(peekIs(q, tricky));
}

static void test_queue_ack_beyond_file() {
  FakeStore st;
  st.data = rec(1) + "\n";
  st.has_ack = true; st.ack = 999;
  RxQueue q(st, 4096);
  q.begin();
  CHECK(st.resets == 1);
  CHECK(q.pending() == 0);
  CHECK(q.push(rec(2).c_str()));
  CHECK(st.data == rec(2) + "\n");
}

static void test_queue_ack_mid_line() {
  FakeStore st;
  st.data = rec(1) + "\n" + rec(2) + "\n";
  st.has_ack = true; st.ack = 3;       // points into the first record
  RxQueue q(st, 4096);
  q.begin();
  CHECK(q.pending() == 1);
  CHECK(peekIs(q, rec(2)));
}

static void test_queue_append_fails() {
  FakeStore st;
  RxQueue q(st, 4096);
  q.begin();
  q.push(rec(1).c_str());
  st.fail_append = true;
  CHECK(!q.push(rec(2).c_str()));
  CHECK(q.dropped() == 1);
  st.fail_append = false;
  CHECK(q.push(rec(3).c_str()));
  CHECK(peekIs(q, rec(1))); q.ack();
  CHECK(peekIs(q, rec(3)));
}

static void test_queue_partial_append_does_not_corrupt_the_next() {
  FakeStore st;
  RxQueue q(st, 4096);
  q.begin();
  q.push(rec(1).c_str());
  st.partial_append = 4;               // the filesystem wrote 4 bytes and then failed
  CHECK(!q.push(rec(2).c_str()));
  st.partial_append = 0;
  CHECK(q.push(rec(3).c_str()));
  CHECK(q.pending() == 2);
  CHECK(peekIs(q, rec(1))); q.ack();
  CHECK(peekIs(q, rec(3)));
}

static void test_queue_line_too_long_for_peek() {
  FakeStore st;
  std::string big = "p{\"x\":\"" + std::string(600, 'a') + "\"}";
  st.data = big + "\n" + rec(1) + "\n";
  RxQueue q(st, 4096);
  q.begin();
  CHECK(peekIs(q, rec(1)));
  CHECK(q.dropped() == 1);
  CHECK(q.pending() == 1);
}

static void test_queue_push_rejects_bad_lines() {
  FakeStore st;
  RxQueue q(st, 4096);
  q.begin();
  CHECK(!q.push(""));
  CHECK(!q.push("x{\"n\":1}"));                              // unknown kind
  CHECK(!q.push("p{\"n\":1"));                               // not a complete object
  CHECK(!q.push(("p{\"x\":\"" + std::string(1100, 'a') + "\"}").c_str()));   // longer than a line may be
  CHECK(st.data.empty());
  CHECK(q.dropped() == 4);
}

static void test_queue_holds_a_maximum_size_packet() {
  uint8_t raw[255];
  for (int i = 0; i < 255; ++i) raw[i] = (uint8_t)i;
  const std::string pubkey(64, 'a'), name(31, 'N');
  RxPacket p{};
  p.epoch = 1794518777u; p.snr = -12.75f; p.rssi = -120; p.raw = raw; p.len = 255;
  p.fix = {-51.12345, -104.54321, 99.9f};
  char line[1100];
  line[0] = 'p';
  const size_t n = rxBuildPacketJson(line + 1, sizeof line - 1, pubkey.c_str(), name.c_str(), p);
  CHECK(n > 0);
  FakeStore st;
  RxQueue q(st, 4096);
  q.begin();
  CHECK(q.push(line));
  char out[1100];
  CHECK(q.peek(out, sizeof out));
  CHECK(strcmp(out, line) == 0);
}

#include "pump_tests.inc"

int main() {
  run_pump_tests();
  test_queue_fifo();
  test_queue_peek_is_stable();
  test_queue_ack_without_peek_is_ignored();
  test_queue_cap_drops_new();
  test_queue_reset_when_empty();
  test_queue_restart_resumes();
  test_queue_torn_last_line();
  test_queue_torn_line_that_ends_in_brace();
  test_queue_brace_inside_string_is_not_structure();
  test_queue_ack_beyond_file();
  test_queue_ack_mid_line();
  test_queue_append_fails();
  test_queue_partial_append_does_not_corrupt_the_next();
  test_queue_line_too_long_for_peek();
  test_queue_push_rejects_bad_lines();
  test_queue_holds_a_maximum_size_packet();
  test_distance();
  test_no_fix_no_capture();
  test_clock_invalid_no_capture();
  test_fix_lost_stops_capture();
  test_moving_captures();
  test_stationary_after_dwell();
  test_stationary_ends_on_move();
  test_rf_first_sample_at_once();
  test_rf_due_by_distance();
  test_rf_due_by_time();
  test_rf_due_parked();
  test_discover_30s();
  test_discover_backoff();
  test_discover_backoff_before_first();
  test_discover_not_when_stationary();
  test_millis_wrap();
  test_packet_json();
  test_packet_json_escapes_name();
  test_packet_json_control_char_in_name();
  test_packet_json_no_name();
  test_packet_json_too_small();
  test_packet_json_exact_fit();
  test_rf_json();
  test_topic();
  test_iso();
  if (g_failed) { printf("rxcapture: %d check(s) failed\n", g_failed); return 1; }
  printf("rxcapture: all passed\n");
  return 0;
}
