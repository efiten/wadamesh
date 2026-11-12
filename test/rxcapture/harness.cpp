// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for src/helpers/rxcapture. Run through test/rxcapture/run.sh.
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

#include "RxRecord.h"
#include "RxGate.h"

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

int main() {
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
