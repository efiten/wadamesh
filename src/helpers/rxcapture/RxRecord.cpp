// SPDX-License-Identifier: GPL-3.0-or-later
#include "RxRecord.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {

// Appends to a fixed buffer and remembers an overflow, so a builder checks once at the end.
struct Out {
  char* buf;
  size_t cap;
  size_t len = 0;
  bool ok = true;

  void put(const char* s, size_t n) {
    if (!ok) return;
    if (len + n + 1 > cap) { ok = false; return; }
    memcpy(buf + len, s, n);
    len += n;
    buf[len] = 0;
  }
  void put(const char* s) { put(s, strlen(s)); }
  void fmt(const char* f, ...) {
    char tmp[96];
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(tmp, sizeof tmp, f, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof tmp) { ok = false; return; }
    put(tmp, (size_t)n);
  }
  void jsonString(const char* s) {
    put("\"");
    for (; *s; ++s) {
      const unsigned char c = (unsigned char)*s;
      if (c == '"' || c == '\\') { const char e[2] = {'\\', (char)c}; put(e, 2); }
      else if (c < 0x20) fmt("\\u%04x", c);
      else put((const char*)&c, 1);
    }
    put("\"");
  }
  size_t done() {
    if (!ok) { if (cap) buf[0] = 0; return 0; }
    return len;
  }
};

void head(Out& o, const char* pubkey_hex, const char* name, uint32_t epoch) {
  char ts[21];
  rxIso8601(ts, epoch);
  o.put("{\"origin_id\":");
  o.jsonString(pubkey_hex);
  if (name && name[0]) { o.put(",\"origin\":"); o.jsonString(name); }
  o.fmt(",\"timestamp\":\"%s\"", ts);
}

void gps(Out& o, const RxFix& f) {
  o.fmt("\"gps\":{\"lat\":%.5f,\"lon\":%.5f,\"acc_m\":%.1f}", f.lat, f.lon, (double)f.acc_m);
}

}  // namespace

void rxIso8601(char out[21], uint32_t epoch) {
  const uint32_t secs = epoch % 86400u;
  // Days since 1970-01-01 to a civil date (Howard Hinnant's days_from_civil inverse).
  int64_t z = (int64_t)(epoch / 86400u) + 719468;
  const int64_t era = z / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  const int y = (int)(yoe + era * 400) + (m <= 2 ? 1 : 0);
  // A uint32 epoch ends in 2106, so the text is always 20 characters; the wider
  // scratch buffer is only there because the compiler cannot see that.
  char tmp[48];
  snprintf(tmp, sizeof tmp, "%04d-%02u-%02uT%02u:%02u:%02uZ", y, m, d,
           (unsigned)(secs / 3600u), (unsigned)(secs / 60u % 60u), (unsigned)(secs % 60u));
  memcpy(out, tmp, 20);
  out[20] = 0;
}

size_t rxBuildPacketJson(char* out, size_t cap, const char* pubkey_hex, const char* name, const RxPacket& p) {
  static const char kHex[] = "0123456789abcdef";
  Out o{out, cap};
  head(o, pubkey_hex, name, p.epoch);
  o.put(",\"type\":\"PACKET\",\"direction\":\"rx\",\"raw\":\"");
  for (int i = 0; i < p.len; ++i) {
    const char h[2] = {kHex[p.raw[i] >> 4], kHex[p.raw[i] & 0x0F]};
    o.put(h, 2);
  }
  o.fmt("\",\"SNR\":%.2f,\"RSSI\":%d,", (double)p.snr, p.rssi);
  gps(o, p.fix);
  o.put("}");
  return o.done();
}

size_t rxBuildRfJson(char* out, size_t cap, const char* pubkey_hex, const char* name, const RxRfSample& s) {
  Out o{out, cap};
  head(o, pubkey_hex, name, s.epoch);
  o.put(",\"type\":\"RF_SAMPLE\",");
  gps(o, s.fix);
  o.fmt(",\"stationary\":%s", s.stationary ? "true" : "false");
  o.fmt(",\"uptime_secs\":%lu,\"battery_mv\":%u,\"queue_len\":%u,\"errors\":%u",
        (unsigned long)s.uptime_secs, (unsigned)s.battery_mv, (unsigned)s.queue_len, (unsigned)s.errors);
  o.fmt(",\"noise_floor\":%d,\"last_rssi\":%d,\"last_snr\":%.2f",
        (int)s.noise_floor, (int)s.last_rssi, (double)s.last_snr);
  o.fmt(",\"tx_air_secs\":%lu,\"rx_air_secs\":%lu,\"recv\":%lu,\"sent\":%lu",
        (unsigned long)s.tx_air_secs, (unsigned long)s.rx_air_secs, (unsigned long)s.recv, (unsigned long)s.sent);
  o.fmt(",\"flood_rx\":%lu,\"direct_rx\":%lu,\"flood_tx\":%lu,\"direct_tx\":%lu,\"recv_errors\":%lu}",
        (unsigned long)s.flood_rx, (unsigned long)s.direct_rx, (unsigned long)s.flood_tx,
        (unsigned long)s.direct_tx, (unsigned long)s.recv_errors);
  return o.done();
}

size_t rxTopic(char* out, size_t cap, const char* pubkey_hex, char kind) {
  const char* suffix = kind == 'p' ? "packets" : kind == 'r' ? "rf" : nullptr;
  if (!suffix) return 0;
  const int n = snprintf(out, cap, "meshcore/client/%s/%s", pubkey_hex, suffix);
  return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}
