// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(ESP32) && defined(RX_CAPTURE)

#include "RxTransport.h"

#include <WiFi.h>
#include <string.h>

#include "DriveMode.h"
#include "RxCapture.h"
#include "SdNvsPrefs.h"

#if RX_TLS
#include "RxCaCerts.h"
#endif

RxTransport rxTransport;

const char* rxLinkName(RxLink link) {
  switch (link) {
    case RxLink::Off:            return "off";
    case RxLink::NoConfig:       return "not set up";
    case RxLink::WaitingForWifi: return "waiting for Wi-Fi";
    case RxLink::Connecting:     return "connecting";
    case RxLink::Connected:      return "connected";
    case RxLink::Failed:         return "failed";
  }
  return "?";
}

void RxTransport::begin(const char* pubkey_hex) {
  strncpy(_pubkey, pubkey_hex, sizeof(_pubkey) - 1);
  reloadConfig();
}

void RxTransport::reloadConfig() {
  _url[0] = _user[0] = _pwd[0] = 0;
  SdNvsPrefs p;
  if (p.begin("rxcap", true)) {
    if (p.isKey("url"))  p.getString("url",  _url,  sizeof(_url));
    if (p.isKey("user")) p.getString("user", _user, sizeof(_user));
    if (p.isKey("pwd"))  p.getString("pwd",  _pwd,  sizeof(_pwd));
    p.end();
  }
  if (_client) destroyClient();   // the next loop() pass reconnects with the new values
  _retry_at_ms = millis();
}

void RxTransport::saveConfig(const char* url, const char* user, const char* pwd) {
  SdNvsPrefs p;
  if (!p.begin("rxcap", false)) return;
  p.putString("url",  url);
  p.putString("user", user);
  p.putString("pwd",  pwd);
  p.end();
}

bool RxTransport::configOk() const {
#if RX_TLS
  return strncmp(_url, "wss://", 6) == 0 && _url[6] != 0;
#else
  return strncmp(_url, "mqtt://", 7) == 0 && _url[7] != 0;
#endif
}

void RxTransport::start() {
  _want = true;
  _retry_at_ms = millis();
}

void RxTransport::stop() {
  _want = false;
  destroyClient();
  _link = RxLink::Off;
}

// Runs on the esp-mqtt task: it only sets flags the main loop reads.
void RxTransport::onEvent(void* arg, esp_event_base_t, int32_t id, void* data) {
  RxTransport* self = static_cast<RxTransport*>(arg);
  esp_mqtt_event_handle_t ev = static_cast<esp_mqtt_event_handle_t>(data);
  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:    self->_connected = true; break;
    case MQTT_EVENT_DISCONNECTED: self->_connected = false; self->_dropped = true; break;
    case MQTT_EVENT_PUBLISHED:    self->_published_id = ev->msg_id; break;
    case MQTT_EVENT_ERROR:
      if (ev->error_handle) {
        self->_last_error = ev->error_handle->esp_tls_last_esp_err
                              ? ev->error_handle->esp_tls_last_esp_err
                              : (int)ev->error_handle->connect_return_code;
      }
      break;
    default: break;
  }
}

void RxTransport::createClient() {
  esp_mqtt_client_config_t cfg = {};
  cfg.uri = _url;
  cfg.client_id = _pubkey;
  cfg.username = _user;
  cfg.password = _pwd;
  cfg.keepalive = 60;
  cfg.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
  cfg.disable_auto_reconnect = true;   // a drop ends this client; loop() builds a new one
  cfg.buffer_size = 1024;
  cfg.network_timeout_ms = 10000;
#if RX_TLS
  cfg.cert_pem = kRxCaCertsPem;
#endif
  _connected = false;
  _dropped = false;
  _published_id = -1;
  _inflight = -1;
  _client = esp_mqtt_client_init(&cfg);
  if (!_client) { _retry_at_ms = millis() + kRetryMs; _link = RxLink::Failed; return; }
  esp_mqtt_client_register_event(_client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, &RxTransport::onEvent, this);
  if (esp_mqtt_client_start(_client) != ESP_OK) {
    destroyClient();
    _retry_at_ms = millis() + kRetryMs;
    _link = RxLink::Failed;
    return;
  }
  _link = RxLink::Connecting;
}

void RxTransport::destroyClient() {
  if (_client) {
    esp_mqtt_client_destroy(_client);
    _client = nullptr;
  }
  _connected = false;
  _dropped = false;
  if (_inflight >= 0) { _inflight = -1; _failed_flag = true; _failures++; }
}

void RxTransport::loop() {
  if (!_want) return;
  if (!configOk()) { _link = RxLink::NoConfig; return; }
  const uint32_t now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (_client) destroyClient();
    _link = RxLink::WaitingForWifi;
    _retry_at_ms = now;
    return;
  }
  if (_client && _dropped) {
    destroyClient();
    _retry_at_ms = now + kRetryMs;
    _link = RxLink::Failed;
    return;
  }
  if (_client && _inflight >= 0 && (uint32_t)(now - _inflight_ms) >= kPublishTimeoutMs) {
    destroyClient();   // a dead socket never acks: start over
    _retry_at_ms = now + kRetryMs;
    _link = RxLink::Failed;
    return;
  }
  if (!_client) {
    if ((int32_t)(now - _retry_at_ms) >= 0) createClient();
    return;
  }
  _link = _connected ? RxLink::Connected : RxLink::Connecting;
}

bool RxTransport::canPublish() const {
  return _client && _connected && !_dropped && _inflight < 0;
}

bool RxTransport::publish(const char* topic, const char* json) {
  if (!canPublish()) return false;
  const int id = esp_mqtt_client_enqueue(_client, topic, json, 0, 1, 0, true);
  if (id < 0) return false;
  _inflight = id;
  _inflight_ms = millis();
  return true;
}

bool RxTransport::takeAck() {
  if (_inflight < 0 || _published_id != _inflight) return false;
  _inflight = -1;
  _sent++;
  return true;
}

bool RxTransport::takeFailure() {
  if (!_failed_flag) return false;
  _failed_flag = false;
  return true;
}

void RxTransport::formatStatus(char* out, size_t cap) const {
  snprintf(out, cap, "rx: %s, sent %lu, failed %lu, last error %d, tls %d, id %.12s, url %s",
           rxLinkName(_link), (unsigned long)_sent, (unsigned long)_failures, (int)_last_error,
           (int)RX_TLS, _pubkey, _url[0] ? _url : "(none)");
}

// ---- serial CLI -----------------------------------------------------------

bool rxConfigSet(const char* key, const char* value) {
  char url[128], user[48], pwd[96];
  strncpy(url, rxTransport.url(), sizeof(url)); url[sizeof(url) - 1] = 0;
  strncpy(user, rxTransport.user(), sizeof(user)); user[sizeof(user) - 1] = 0;
  pwd[0] = 0;
  {
    SdNvsPrefs p;
    if (p.begin("rxcap", true)) {
      if (p.isKey("pwd")) p.getString("pwd", pwd, sizeof(pwd));
      p.end();
    }
  }
  char* dst = nullptr; size_t cap = 0;
  if (strcmp(key, "url") == 0)       { dst = url;  cap = sizeof(url); }
  else if (strcmp(key, "user") == 0) { dst = user; cap = sizeof(user); }
  else if (strcmp(key, "pwd") == 0)  { dst = pwd;  cap = sizeof(pwd); }
  if (!dst || strlen(value) >= cap) return false;
  strcpy(dst, value);
  RxTransport::saveConfig(url, user, pwd);
  rxTransport.reloadConfig();
  return true;
}

void rxCliSet(const char* kv) {
  const char* sp = strchr(kv, ' ');
  char key[8] = {};
  if (sp && (size_t)(sp - kv) < sizeof(key)) memcpy(key, kv, (size_t)(sp - kv));
  const bool known = strcmp(key, "url") == 0 || strcmp(key, "user") == 0 || strcmp(key, "pwd") == 0;
  if (!known) { Serial.println("  Error: use set rx.url|rx.user|rx.pwd <value>"); return; }
  if (!rxConfigSet(key, sp + 1)) { Serial.println("  Error: value too long (url 127, user 47, pwd 95 characters)"); return; }
  Serial.println("  > OK");
}

bool rxCliCommand(const char* line) {
  if (strcmp(line, "rx status") == 0) {
    char s[256];
#if defined(RX_CAPTURE_SELFTEST)
    {
      // Resolve the broker host from the configured URL (scheme://host[:port][/path]).
      char host[96] = {};
      const char* h = strstr(rxTransport.url(), "://");
      if (h) {
        h += 3;
        size_t n = strcspn(h, ":/");
        if (n >= sizeof(host)) n = sizeof(host) - 1;
        memcpy(host, h, n);
      }
      IPAddress resolved;
      const int r = host[0] ? WiFi.hostByName(host, resolved) : 0;
      Serial.printf("  > wifi: status %d ip %s gw %s dns %s rssi %d, lookup %d -> %s\n", (int)WiFi.status(),
                    WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(),
                    WiFi.dnsIP().toString().c_str(), (int)WiFi.RSSI(), r, resolved.toString().c_str());
    }
#endif
    rxTransport.formatStatus(s, sizeof(s));
    Serial.printf("  > %s\n", s);
    rxCapture.formatStatus(s, sizeof(s));
    Serial.printf("  > %s\n", s);
    return true;
  }
  if (strcmp(line, "get rx.url") == 0)  { Serial.printf("  > %s\n", rxTransport.url());  return true; }
  if (strcmp(line, "get rx.user") == 0) { Serial.printf("  > %s\n", rxTransport.user()); return true; }
#if defined(RX_CAPTURE_SELFTEST)
  if (strcmp(line, "rx test") == 0) {
    char topic[112];
    snprintf(topic, sizeof(topic), "meshcore/client/%s/packets", rxTransport.pubkey());
    const bool ok = rxTransport.publish(topic, "{\"type\":\"TEST\"}");
    Serial.printf("  > rx test: %s\n", ok ? "queued" : "not connected");
    return true;
  }
#endif
  return false;
}

#endif
