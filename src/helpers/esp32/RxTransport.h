// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#if defined(ESP32) && defined(RX_CAPTURE)

// MQTT session to the CoreScope broker for RX capture: MQTT 3.1.1, clientId =
// the node's public key, QoS 1, one publish in flight at a time.
//
// With RX_TLS=1 the URL must be wss://host:port/path and the broker certificate
// is verified against the roots in RxCaCerts.h. With RX_TLS=0 it must be
// mqtt://host:port and nothing is encrypted.
//
// The session is rebuilt from scratch after every drop, so esp-mqtt never
// re-sends a message on its own: the caller keeps a record until takeAck() and
// decides what to do with it on takeFailure().

#include <Arduino.h>
#include <mqtt_client.h>

enum class RxLink : uint8_t { Off, NoConfig, WaitingForWifi, Connecting, Connected, Failed };

const char* rxLinkName(RxLink link);

class RxTransport {
public:
  void begin(const char* pubkey_hex);   // loads the settings; does not connect
  void start();                         // drive mode on
  void stop();                          // drive mode off
  void loop();                          // every pass of the main loop

  RxLink link() const { return _link; }
  bool canPublish() const;              // connected and nothing in flight
  bool publish(const char* topic, const char* json);
  bool takeAck();                       // true once when the publish in flight got its PUBACK
  bool takeFailure();                   // true once when it timed out or the link dropped under it

  const char* pubkey() const { return _pubkey; }
  uint32_t sent() const { return _sent; }

  static void saveConfig(const char* url, const char* user, const char* pwd);
  void reloadConfig();
  void formatStatus(char* out, size_t cap) const;
  const char* url() const { return _url; }
  const char* user() const { return _user; }

private:
  static const uint32_t kPublishTimeoutMs = 8000;   // coredrive-rx publisher.js publish()
  static const uint32_t kRetryMs = 10000;

  static void onEvent(void* arg, esp_event_base_t base, int32_t id, void* data);
  bool configOk() const;
  void createClient();
  void destroyClient();

  char _pubkey[65] = {};
  char _url[128] = {};
  char _user[48] = {};
  char _pwd[96] = {};
  bool _want = false;
  RxLink _link = RxLink::Off;
  esp_mqtt_client_handle_t _client = nullptr;
  uint32_t _retry_at_ms = 0;
  int _inflight = -1;                 // msg id of the publish awaiting its PUBACK
  uint32_t _inflight_ms = 0;
  bool _failed_flag = false;
  uint32_t _sent = 0, _failures = 0;
  // Written by the esp-mqtt task, read by the main loop.
  volatile bool _connected = false;
  volatile bool _dropped = false;
  volatile int _published_id = -1;
  volatile int _last_error = 0;
};

extern RxTransport rxTransport;

// Serial CLI glue (MyMesh::checkCLIRescueCmd). rxCliSet handles the part after
// "set rx."; rxCliCommand returns false when the line is not an rx command.
void rxCliSet(const char* key_value);
bool rxCliCommand(const char* line);

#endif
