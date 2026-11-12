# RX Capture on the T-LoRa Pager Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The Pager publishes every packet it receives, with SNR, RSSI and its own GNSS position, to CoreScope over MQTT, in the payload coredrive-rx sends, with a file-backed queue for when there is no Wi-Fi.

**Architecture:** Pure logic (payload builders, gates, queue) lives in `src/helpers/rxcapture/` with no Arduino includes, so it is tested on the host. ESP32 glue (`RxCapture`, `RxTransport`, `DriveMode`) lives in `src/helpers/esp32/` and is compiled only when `RX_CAPTURE=1`, which only the two `tlora_pager_*` envs set. `MqttBridge` is not touched.

**Tech Stack:** PlatformIO, arduino-esp32 (platform `espressif32@6.11.0`), esp-mqtt from the SDK (`tools/sdk/esp32s3/include/mqtt/esp-mqtt`), LVGL (existing UI), host tests with `c++ -std=c++17` driven by a `run.sh` (the pattern of `test/history_segments/run.sh`).

**Spec:** `docs/superpowers/specs/2026-11-12-rx-capture-design.md`

## Global Constraints

- Branch `feat/rx-capture`, from `upstream/main`. Fork only, no pull request to upstream.
- `git add <specific files>` only. One commit per task. No force-push.
- Everything new is behind `#if defined(RX_CAPTURE)`. Other envs must build byte-identical logic.
- Topics: `meshcore/client/<pubkey>/packets` and `meshcore/client/<pubkey>/rf`. `<pubkey>` is the 64-char lowercase hex of the node's public key. MQTT 3.1.1, clientId = `<pubkey>`, QoS 1, keepalive 60 s.
- The `/packets` and `/rf` payload field names and types are fixed by `corescope-rx/src/publisher.js:118` and `:133`. Do not add, rename or drop a field.
- A record leaves the queue only after its PUBACK.
- No record without a position fix.
- Drive mode never persists a BLE-off state: it must not call `disableBle()`.
- Never guess a packet or frame format: read `lib/MeshCore` and `src/MyMesh.cpp`.
- Build command: `PYTHONUTF8=1 pio run -e tlora_pager_sx1262_companion_radio_touch` (on Windows PlatformIO cannot read the partition CSV without UTF-8 mode). Host tests: `bash test/rxcapture/run.sh`.
- Both Pager envs use `variants/lilygo_tlora_pager/partitions_tlora_pager_rxcapture.csv` (5 MB app slots, 5242880 bytes). The stock table is not edited.
- Flashing the device, and anything sent to the live broker, needs the user's confirmation each time. Use the staging broker for tests.

## Review Focus

1. Power cut while the queue file is being appended or its ack offset is written: after reboot every fully written record is still published once, and a half-written last line is skipped, not sent as garbage. (Task 4)
2. The RTC has no valid time yet (no GNSS time, no NTP): records must not be stamped 1970 or 2000. No record is made until the clock holds a real time. (Task 3)
3. A packet arrives in `logRxRaw()` while the main loop is draining the queue: the hook only copies into a ring buffer and never touches the file or the MQTT client. (Task 6)
4. The broker accepts the connection but the ACL silently drops publishes (MQTT 3.1.1 PUBACK carries no reason code): this cannot be detected on the device. The status screen says "sent", never "stored". (Task 7)
5. Drive mode is left by a reboot or a crash, not by the stop button: BLE and both servers come back because nothing was persisted. (Task 1)

## File Structure

| File | Responsibility |
|---|---|
| `src/helpers/rxcapture/RxRecord.h/.cpp` | Build the `/packets` and `/rf` JSON payloads and topic strings. Pure. |
| `src/helpers/rxcapture/RxGate.h/.cpp` | Decide: capture or not (fix, clock, stationary), when an RF sample is due, when a discover is due. Pure. |
| `src/helpers/rxcapture/RxQueue.h/.cpp` | Append-only line queue with an ack offset and a size cap, over an `RxStore` interface. Pure. |
| `src/helpers/esp32/DriveMode.h/.cpp` | Enter and leave drive mode, heap log per step. |
| `src/helpers/esp32/RxTransport.h/.cpp` | esp-mqtt session: connect, publish QoS 1, report PUBACK. |
| `src/helpers/esp32/RxCapture.h/.cpp` | Wiring: RX hook ring buffer, GNSS, counters, queue store on SD or internal FS, settings, main-loop tick. |
| `src/helpers/esp32/MultiTransportCompanionInterface.h/.cpp` | Two new methods: suspend and restore for drive mode. |
| `src/MyMesh.cpp` | One call in `logRxRaw()`, serial commands `drive ...` and `rx ...`. |
| `src/main.cpp` | `rxCapture.begin()` after `the_mesh.begin()`, `rxCapture.loop()` in `loop()`. |
| `src/ui-touch/UITask.cpp` | Drive page and its app tile. |
| `platformio.ini` | `-D RX_CAPTURE=1` and `+<helpers/rxcapture/*.cpp>` in both `tlora_pager_*` envs. |
| `test/rxcapture/run.sh`, `harness.cpp` | Host tests for the three pure units. |

---

### Task 1: Drive mode and the heap measurement (decides the transport)

**Files:**
- Create: `src/helpers/esp32/DriveMode.h`, `src/helpers/esp32/DriveMode.cpp`
- Modify: `src/helpers/esp32/MultiTransportCompanionInterface.h`, `.cpp` (next to `prepareForHttpOta()` at `.cpp:438`)
- Modify: `src/MyMesh.cpp` (`checkCLIRescueCmd()`, next to the `mqtt.status` branch at `:6130`)
- Modify: `src/ui-touch/UITask.cpp` (two exported functions, see step 3)
- Modify: `platformio.ini` (both `[env:tlora_pager_*_companion_radio_touch]`)

**Interfaces:**
- Produces:
  - `void MultiTransportCompanionInterface::suspendForDriveMode();` stops the TCP and WS servers and deinitialises NimBLE, remembering what was running. Does not call `wifiConfigSetBleEnabled()`.
  - `void MultiTransportCompanionInterface::restoreAfterDriveMode();`
  - `namespace DriveMode { bool enter(); void leave(); bool active(); }`
  - `void uiDriveModeFreeHeavy();` and `void uiDriveModeSetLocked(bool locked);` in `UITask.cpp`, declared in `DriveMode.cpp` as `extern`.
  - Serial commands `drive on`, `drive off`, `drive status`.

- [ ] **Step 1: Back up the firmware now on the device and identify its radio**

Ask the user before running. Run: `pio pkg exec -p tool-esptoolpy -- esptool.py --port COM15 read_flash 0 0x1000000 <scratchpad>/pager-backup.bin`
Then: `strings pager-backup.bin | grep -E "tlora_pager_(sx1262|lr1121)"`
Expected: exactly one env name. Use that env for every build in this plan. If neither string is present, ask the user which radio the unit has.

- [ ] **Step 2: Add `suspendForDriveMode()` / `restoreAfterDriveMode()`**

Model them on `prepareForHttpOta()` (`.cpp:438-497`) and `restoreAfterHttpOta()` (`.cpp:505`), with these differences: stop both servers unconditionally (`stopTcpServer()`, `stopWsServer()`), use their own `_drive_*` flags (not the `_ota_*` ones), and emit no `meshcoreRepeaterTcpOtaEmitLine`. Keep the `bleDetachServerCallbacks()` and `BleKbd::stackGoingDown()` calls before `NimBLEDevice::deinit(true)`, in that order.

- [ ] **Step 3: Add the two UI hooks in `UITask.cpp`**

`uiDriveModeFreeHeavy()` calls `freeMapTiles()` (`:39722`), stops audio through the path `luaHostAudioStop()` uses (`:75936`) and closes a running Lua app. Find the function the app drawer uses to leave a Lua app and call that; if no such entry point exists, add one next to `LuaAppHost` and say so in the commit message. `uiDriveModeSetLocked(true)` makes the Map, Lua app and audio entry points return to the Drive page with a toast "Not available in drive mode".

- [ ] **Step 4: Implement `DriveMode`**

`enter()` runs, in order: `suspendForDriveMode()`, `uiDriveModeFreeHeavy()`, `uiDriveModeSetLocked(true)`. Before the first and after each step it prints one line in the format of `pagerLogInternalHeap()` (`src/main.cpp:134`), with phases `drive: before`, `drive: after ble+servers`, `drive: after ui`. `leave()` reverses it. State is RAM only.

- [ ] **Step 5: Serial commands and build flag**

`drive on` / `drive off` / `drive status` in `checkCLIRescueCmd()`. `drive status` prints `drive: on|off` plus one heap line. Add `-D RX_CAPTURE=1` to both Pager envs.

- [ ] **Step 6: Build**

Run: `pio run -e <env from step 1>`
Expected: `SUCCESS`. Also run `pio run -e LilyGo_TDeck_companion_radio_touch` and expect `SUCCESS` (proves the guard).

- [ ] **Step 7: Flash and measure (user confirms the flash)**

With Wi-Fi associated and BLE enabled on the device, send `drive on`, wait 10 s, `drive status`, `drive off`, `drive status`.
Record the three `drive:` heap lines and the two status lines in the spec under "Measured on the device".
Expected after `drive off`: a `[ble]` or `[boot] BLE stack ready` line, and the phone can connect again.
Then power-cycle the device while drive mode is on. Expected: BLE and port 8765 are back after boot (Review Focus 5).

- [ ] **Step 8: Decide the transport and write it into the spec**

Rule from the spec: internal free in the `drive: after ui` line of at least 46 KB (46 * 1024 = 47104 bytes) with Wi-Fi associated means `RX_TLS=1` (WSS on 8084, certificate verified). Less means `RX_TLS=0` (plain MQTT on 1883): stop and tell the user, because that needs a listener opened on the broker and sends the password in clear text.

- [ ] **Step 9: Commit**

```bash
git add src/helpers/esp32/DriveMode.h src/helpers/esp32/DriveMode.cpp \
  src/helpers/esp32/MultiTransportCompanionInterface.h src/helpers/esp32/MultiTransportCompanionInterface.cpp \
  src/MyMesh.cpp src/ui-touch/UITask.cpp platformio.ini docs/superpowers/specs/2026-11-12-rx-capture-design.md
git commit -m "pager: drive mode frees BLE, companion servers, map tiles, audio and Lua"
```

---

### Task 2: Payload builders

**Files:**
- Create: `src/helpers/rxcapture/RxRecord.h`, `RxRecord.cpp`
- Create: `test/rxcapture/run.sh`, `test/rxcapture/harness.cpp`

**Interfaces:**
- Produces:

```cpp
struct RxFix { double lat, lon; float acc_m; };
struct RxPacket { uint32_t epoch; float snr; int rssi; const uint8_t* raw; int len; RxFix fix; };
struct RxRfSample {
  uint32_t epoch; RxFix fix; bool stationary;
  uint32_t uptime_secs; uint16_t battery_mv; uint8_t queue_len; uint16_t errors;
  int16_t noise_floor; int8_t last_rssi; float last_snr;
  uint32_t tx_air_secs, rx_air_secs, recv, sent, flood_rx, direct_rx, flood_tx, direct_tx, recv_errors;
};
// Each returns the JSON length, or 0 when it does not fit in cap. No trailing newline.
size_t rxBuildPacketJson(char* out, size_t cap, const char* pubkey_hex, const char* name, const RxPacket& p);
size_t rxBuildRfJson(char* out, size_t cap, const char* pubkey_hex, const char* name, const RxRfSample& s);
// kind: 'p' = packets, 'r' = rf.
size_t rxTopic(char* out, size_t cap, const char* pubkey_hex, char kind);
void rxIso8601(char out[21], uint32_t epoch);   // "2026-11-12T21:04:05Z"
```

- [ ] **Step 1: Write `run.sh` and the failing tests**

`run.sh` compiles `harness.cpp` with every `src/helpers/rxcapture/*.cpp` using `c++ -std=c++17 -Wall -I src/helpers/rxcapture` and runs it; exit code 0 and a last line `rxcapture: all passed`. Tests, with plain `assert` and `strcmp`:

```cpp
// test_packet_json
uint8_t raw[] = {0x11, 0x00, 0xAB};
RxPacket p{1794518777u, -7.25f, -101, raw, 3, {51.12345, 4.54321, 12.5f}};
rxBuildPacketJson(buf, sizeof buf, "ab12", "ON8AR pager", p);
assert(!strcmp(buf,
  "{\"origin_id\":\"ab12\",\"origin\":\"ON8AR pager\",\"timestamp\":\"2026-11-12T21:26:17Z\","
  "\"type\":\"PACKET\",\"direction\":\"rx\",\"raw\":\"1100ab\",\"SNR\":-7.25,\"RSSI\":-101,"
  "\"gps\":{\"lat\":51.12345,\"lon\":4.54321,\"acc_m\":12.5}}"));
// test_packet_json_escapes_name: name `a"b\c` appears as `a\"b\\c`
// test_packet_json_no_name: name "" omits the "origin" key entirely
// test_packet_json_too_small: cap 32 returns 0
// test_rf_json: every field of RxRfSample present under the names in publisher.js:133-160,
//   "type":"RF_SAMPLE", "stationary":true|false as JSON booleans
// test_topic: rxTopic(.., "ab12", 'p') == "meshcore/client/ab12/packets"; 'r' == ".../rf"
// test_iso: rxIso8601(1794518777) == "2026-11-12T21:26:17Z"; rxIso8601(951782400) == "2000-02-29T00:00:00Z"
```

Before writing the expected timestamp, confirm it: `date -u -d @1794518777 +%FT%TZ`.

- [ ] **Step 2: Run to see it fail**

Run: `bash test/rxcapture/run.sh`
Expected: compile error, `RxRecord.h` not found.

- [ ] **Step 3: Implement the four functions in `RxRecord.cpp`**

`snprintf` only, no JSON library, no heap. `raw` is lowercase hex. `lat`/`lon` use `%.5f`, `SNR` `%.2f`, `acc_m` `%.1f`.

- [ ] **Step 4: Run to see it pass**

Run: `bash test/rxcapture/run.sh`
Expected: `rxcapture: all passed`

- [ ] **Step 5: Commit**

```bash
git add src/helpers/rxcapture/RxRecord.h src/helpers/rxcapture/RxRecord.cpp test/rxcapture/run.sh test/rxcapture/harness.cpp
git commit -m "rxcapture: build the /packets and /rf payloads coredrive-rx sends"
```

---

### Task 3: Gates and schedules

**Files:**
- Create: `src/helpers/rxcapture/RxGate.h`, `RxGate.cpp`
- Modify: `test/rxcapture/harness.cpp`

**Interfaces:**
- Consumes: `RxFix` from Task 2.
- Produces:

```cpp
double rxDistanceM(const RxFix& a, const RxFix& b);          // haversine, R = 6371000
class RxGate {
public:
  // Call on every new fix. now_ms is a monotonic millisecond clock.
  void onFix(const RxFix& fix, uint32_t now_ms);
  void onNoFix();
  void setClockValid(bool valid);                             // epoch >= 1767225600 (2026-01-01)
  bool captureAllowed() const;                                // fix && clock valid && !stationary
  bool stationary() const;
  bool rfSampleDue(uint32_t now_ms);                          // true once per due sample
  bool discoverDue(uint32_t now_ms);                          // true once per due request
  void onTrafficHeard(uint32_t now_ms);
};
```

Constants, all from coredrive-rx: stationary = within 75 m of the anchor for 300000 ms (`motion.js`); RF sample every 100 m moved or 15000 ms while moving, every 300000 ms while stationary (`rfstats.js:114-116`); discover at most every 30000 ms, held back 15000 ms after traffic, never while stationary (`docs/user-guide.md:57`).

- [ ] **Step 1: Write the failing tests**

```cpp
// test_no_fix_no_capture: fresh gate, clock valid -> captureAllowed() == false
// test_clock_invalid_no_capture: fix given, setClockValid(false) -> false   (Review Focus 2)
// test_moving_captures: fixes 200 m apart every 10 s -> captureAllowed() stays true
// test_stationary_after_dwell: same fix at t=0 and t=300000 -> stationary() == true, captureAllowed() == false
// test_stationary_ends_on_move: then a fix 76 m away -> stationary() == false
// test_rf_due_by_distance: moving, 100 m since last sample at +3 s -> rfSampleDue() true, then false
// test_rf_due_by_time: moving 10 m in 15000 ms -> true
// test_rf_due_parked: stationary -> due once per 300000 ms
// test_discover_30s: moving, no traffic -> due at t=0, not at t=29999, due at t=30000
// test_discover_backoff: onTrafficHeard(t=29000) -> not due at t=30000, due at t=44000
// test_discover_not_when_stationary
// test_millis_wrap: now_ms wrapping past 0xFFFFFFFF does not fire or block a schedule
```

- [ ] **Step 2: Run to see it fail**

Run: `bash test/rxcapture/run.sh`
Expected: compile error, `RxGate.h` not found.

- [ ] **Step 3: Implement `RxGate`**

Port `updateMotion()` from `corescope-rx/src/motion.js` as is. All time comparisons as `(uint32_t)(now - then) >= interval`.

- [ ] **Step 4: Run to see it pass**

Run: `bash test/rxcapture/run.sh`
Expected: `rxcapture: all passed`

- [ ] **Step 5: Commit**

```bash
git add src/helpers/rxcapture/RxGate.h src/helpers/rxcapture/RxGate.cpp test/rxcapture/harness.cpp
git commit -m "rxcapture: capture, RF-sample and discover gates with coredrive-rx timings"
```

---

### Task 4: The queue

**Files:**
- Create: `src/helpers/rxcapture/RxQueue.h`, `RxQueue.cpp`
- Modify: `test/rxcapture/harness.cpp`

**Interfaces:**
- Produces:

```cpp
struct RxStore {                       // implemented by a fake in the harness and by Task 6 on the device
  virtual size_t size() = 0;                                       // bytes in the data file
  virtual bool append(const char* data, size_t n) = 0;             // false = nothing usable written
  virtual size_t readAt(size_t off, char* buf, size_t cap) = 0;
  virtual bool loadAck(size_t* off) = 0;                           // false = no ack file
  virtual bool saveAck(size_t off) = 0;
  virtual bool reset() = 0;                                        // truncate data, ack = 0
  virtual ~RxStore() {}
};
class RxQueue {
public:
  RxQueue(RxStore& store, size_t cap_bytes);
  void begin();
  // line = kind char ('p' or 'r') followed by the JSON, no newline. false = dropped (cap or write error).
  bool push(const char* line);
  // Oldest unacked line into out (NUL-terminated, without the newline). false = queue empty.
  bool peek(char* out, size_t cap);
  void ack();                                                      // the line last returned by peek() is done
  uint32_t pending() const;  uint32_t dropped() const;
};
```

Format: one record per line, `<kind><json>\n`. The ack offset is the byte offset of the first unacked line. When every line is acked, `reset()` is called so the file does not grow forever. `saveAck` runs after every 10 acks and when the queue becomes empty (coredrive-rx `drain.js:23` bounds re-publishing the same way).

- [ ] **Step 1: Write the failing tests against an in-memory `FakeStore`**

```cpp
// test_fifo: push a,b,c -> peek a, ack, peek b, ack, peek c, ack, peek == false, pending()==0
// test_peek_is_stable: peek twice without ack returns the same line
// test_cap_drops_new: cap 64, pushes until false -> dropped()==1, the earlier lines still drain in order
// test_reset_when_empty: after the last ack FakeStore.size()==0 and the next push starts at offset 0
// test_restart_resumes: push 25, ack 12, new RxQueue on the same store -> the next peek is the
//   11th line pushed (the ack offset was last saved after 10 acks), so 2 lines are sent again and none is skipped
// test_torn_last_line: store ends with "p{\"origin" and no newline -> peek skips it, pending() ignores it,
//   and the next push lands on its own line                                   (Review Focus 1)
// test_ack_beyond_file: loadAck returns an offset > size() -> treated as 0 after reset()
// test_append_fails: FakeStore.append returns false -> push returns false, dropped()==1, queue intact
// test_line_too_long_for_peek: a 600-byte line with peek cap 512 -> skipped and counted in dropped()
```

- [ ] **Step 2: Run to see it fail**

Run: `bash test/rxcapture/run.sh`
Expected: compile error, `RxQueue.h` not found.

- [ ] **Step 3: Implement `RxQueue`**

No heap allocation; scan for the newline with `readAt` in chunks of 256 bytes.

- [ ] **Step 4: Run to see it pass**

Run: `bash test/rxcapture/run.sh`
Expected: `rxcapture: all passed`

- [ ] **Step 5: Commit**

```bash
git add src/helpers/rxcapture/RxQueue.h src/helpers/rxcapture/RxQueue.cpp test/rxcapture/harness.cpp
git commit -m "rxcapture: append-only queue with ack offset, size cap and torn-line recovery"
```

---

### Task 5: MQTT transport and settings

**Files:**
- Create: `src/helpers/esp32/RxTransport.h`, `RxTransport.cpp`
- Modify: `src/MyMesh.cpp` (`checkCLIRescueCmd()`)
- Modify: `platformio.ini` (both Pager envs: `+<helpers/rxcapture/*.cpp>`, `-D RX_TLS=<0|1>` from Task 1 step 8)

**Interfaces:**
- Produces:

```cpp
enum class RxLink : uint8_t { Off, NoConfig, WaitingForWifi, Connecting, Connected, Failed };
class RxTransport {
public:
  void begin(const char* pubkey_hex);                // loads settings; does not connect
  void start();  void stop();                        // drive mode on / off
  RxLink link() const;
  bool canPublish() const;                           // Connected and no publish in flight
  bool publish(const char* topic, const char* json); // QoS 1, one in flight at a time
  bool takeAck();                                    // true once when the in-flight publish got its PUBACK
  bool takeFailure();                                // true once when it timed out (8000 ms) or the link dropped
  static void saveConfig(const char* url, const char* user, const char* pwd);
  void formatStatus(char* out, size_t cap) const;
};
```

Settings live in Preferences namespace `rxcap`, keys `url`, `user`, `pwd` (the pattern `MqttBridge` uses with namespace `mqtt`). `url` is `wss://host:port/path` when `RX_TLS=1`, `mqtt://host:port` when `RX_TLS=0`; anything else is `RxLink::NoConfig`.

- [ ] **Step 1: Implement `RxTransport` on esp-mqtt**

`esp_mqtt_client_config_t` with `uri`, `client_id` = pubkey, `keepalive` = 60, `protocol_ver` = MQTT 3.1.1, `disable_auto_reconnect` = false. With `RX_TLS=1` set `cert_pem` to the Let's Encrypt root certificates embedded as a C string in `RxTransport.cpp`. Before embedding, read the broker's chain (`openssl s_client -connect <host>:8084 -showcerts`) and embed the root it ends in plus ISRG Root X1 and X2; name the chain you found in the commit message. Event handler: `MQTT_EVENT_PUBLISHED` with the in-flight `msg_id` sets the ack flag; `MQTT_EVENT_DISCONNECTED` and `MQTT_EVENT_ERROR` set the failure flag when a publish is in flight. The handler runs on the esp-mqtt task: it only sets `volatile` flags, the main loop reads them through `takeAck()` / `takeFailure()`.

- [ ] **Step 2: Serial commands**

`set rx.url <v>`, `set rx.user <v>`, `set rx.pwd <v>`, `get rx.url`, `get rx.user`, `rx status`. `get rx.pwd` is not offered. `rx status` prints `formatStatus()`.

- [ ] **Step 3: Build**

Run: `pio run -e <env from Task 1>`
Expected: `SUCCESS`, and the size line shows the firmware below 5 MB (5242880 bytes, the app slot).

- [ ] **Step 4: Test against the staging broker (user confirms flash and credentials)**

Subscribe on a PC to `meshcore/client/#` on the staging broker. On the device: set the three values, `drive on`, then a temporary `rx test` serial command publishes `{"type":"TEST"}` to `meshcore/client/<pubkey>/packets`.
Expected: the message arrives; `rx status` shows `connected` and `sent 1`. Remove `rx test` before committing.
With `RX_TLS=1` also set `rx.url` to a host with a certificate from another CA. Expected: `rx status` shows `failed` and nothing is published.
Record the `drive status` heap line while connected in the spec.

- [ ] **Step 5: Commit**

```bash
git add src/helpers/esp32/RxTransport.h src/helpers/esp32/RxTransport.cpp src/MyMesh.cpp platformio.ini \
  docs/superpowers/specs/2026-11-12-rx-capture-design.md
git commit -m "rxcapture: MQTT session to the CoreScope broker, QoS 1 with PUBACK tracking"
```

---

### Task 6: Capture wiring

**Files:**
- Create: `src/helpers/esp32/RxCapture.h`, `RxCapture.cpp`
- Modify: `src/MyMesh.cpp` (`logRxRaw()` at `:2336`; a public accessor block for the counters), `src/MyMesh.h`
- Modify: `src/main.cpp` (`begin` after `the_mesh.begin()`, `loop` in `loop()`)

**Interfaces:**
- Consumes: Task 2 builders, Task 3 `RxGate`, Task 4 `RxQueue`/`RxStore`, Task 5 `RxTransport`, Task 1 `DriveMode::active()`.
- Produces:

```cpp
struct RxCaptureStatus {
  bool drive; bool fix; float acc_m; bool stationary; RxLink link;
  uint32_t captured, sent, pending, dropped; float last_snr; bool sd;
};
class RxCapture {
public:
  void begin();
  void loop();
  void onRxRaw(float snr, float rssi, const uint8_t* raw, int len);   // called from MyMesh::logRxRaw
  void getStatus(RxCaptureStatus& out) const;
};
extern RxCapture rxCapture;
// Added to MyMesh, filled from the same sources as the CMD_GET_STATS block (MyMesh.cpp:5527-5575):
void MyMesh::fillRfSample(RxRfSample& s);
```

- [ ] **Step 1: The hook and its ring buffer**

`onRxRaw()` returns at once when `!DriveMode::active()`. Otherwise it copies `(millis, snr, rssi, len, raw)` into a fixed ring of 32 entries of 256 bytes allocated in PSRAM at `begin()`, and calls nothing else (Review Focus 3). A full ring increments `dropped`. Add the one-line call at the top of `MyMesh::logRxRaw()`.

- [ ] **Step 2: `loop()`**

In this order, each pass:
1. Feed the gate: position and validity from the board's location provider (`getLatitude()`, `getLongitude()`, `isValid()` in `src/helpers/WadaNmeaLocationProvider.h:302-310`); `acc_m` = HDOP x 5.0 m, HDOP read from the provider's `nmea` object (MicroNMEA `getHDOP()` returns tenths; add an accessor to `WadaNmeaLocationProvider` if none exists). `setClockValid()` from the RTC epoch.
2. Drain the ring: for each entry call `onTrafficHeard()`; if `captureAllowed()`, build the JSON and send it: straight to `RxTransport::publish()` when `canPublish()` and the queue is empty, otherwise `RxQueue::push()`.
3. If `rfSampleDue()`: `the_mesh.fillRfSample()`, build, same send path with kind `'r'`.
4. If `discoverDue()`: `the_mesh.uiStartDiscoverScan(0)` (`src/MyMesh.h:1035`, 0 = all node types).
5. Publishing state machine: on `takeAck()` ack the queue line if the in-flight record came from the queue; on `takeFailure()` push a live record to the queue (a queued one simply stays). When `canPublish()` and the queue is not empty, `peek()` and publish with the topic from the line's kind char.

A record sent live is held in one static buffer until its PUBACK, so a drop moves it to the queue and it is not lost.

- [ ] **Step 3: The store**

`RxStore` on the SD card when it is mounted (`/rxcap/queue.jsonl`, `/rxcap/queue.ack`), otherwise on the internal data filesystem the firmware already mounts for prefs (same two names under `/rxcap`). Cap: 64 MB on SD, 1 MB internal. Use the filesystem handles the firmware already holds; do not mount anything a second time. `saveAck` writes to `queue.ack.tmp` and renames.

- [ ] **Step 4: `fillRfSample()` and `main.cpp`**

Copy each value from the expression the `CMD_GET_STATS` block uses. `last_snr` is the float, not the x4 integer.

- [ ] **Step 5: Build and host tests**

Run: `pio run -e <env from Task 1>` then `bash test/rxcapture/run.sh`
Expected: `SUCCESS` and `rxcapture: all passed`.

- [ ] **Step 6: Device test on staging (user confirms flash)**

Outdoors or at a window with a fix, `drive on`, second node sends an advert.
Expected on the PC subscriber: one `/packets` message whose `raw` decodes as that advert and whose `gps` is the device's position; an `/rf` message within 15 s while walking, none more often than that when still.
Then switch the Wi-Fi access point off, generate 5 packets while moving, switch it on.
Expected: `rx status` shows `pending 5` then `pending 0`, and exactly 5 messages arrive, oldest first.
Then repeat with a power cycle while `pending` is 5. Expected: 5 messages after reboot and `drive on` (at most 9 duplicates are allowed by the ack interval; with 5 pending and none acked there must be none).

- [ ] **Step 7: Commit**

```bash
git add src/helpers/esp32/RxCapture.h src/helpers/esp32/RxCapture.cpp src/MyMesh.cpp src/MyMesh.h src/main.cpp \
  src/helpers/WadaNmeaLocationProvider.h
git commit -m "rxcapture: publish receptions and RF samples with position, queue them without a link"
```

(Leave `WadaNmeaLocationProvider.h` out of `git add` if no accessor was needed.)

---

### Task 7: Drive page

**Files:**
- Modify: `src/ui-touch/UITask.cpp`

**Interfaces:**
- Consumes: `rxCapture.getStatus()`, `DriveMode::enter()/leave()/active()`, `RxTransport::saveConfig()`.

- [ ] **Step 1: Add the page and its tile**

Model the page on `openDiscoverPage()` (`UITask.cpp:32320`) and the tile on the Discover entry in the app table (`:61068`, hide flag list at `:60162`). Title `Drive`. Guard everything with `#if defined(RX_CAPTURE)`. Content, top to bottom:

```
[ Start drive ]            (label becomes "Stop drive" while active)
GPS    fix 12 m | no fix | parked
Link   connected | waiting for Wi-Fi | connecting | failed | not set up
Sent   1234        Queued 0        Dropped 0
Last   -7.2 dB
Store  SD card | internal (1 MB)
Broker ...                 (opens a sheet with URL, user, password fields)
```

"Sent" counts PUBACKs, and the page says "Sent", never "Stored": the device cannot see what the broker ACL or the ingestor does with a record (Review Focus 4). Refresh once per second while the page is open. All colors from the existing theme helpers, no literal color values.

- [ ] **Step 2: Build**

Run: `pio run -e <env from Task 1>`
Expected: `SUCCESS`

- [ ] **Step 3: Device check (user confirms flash)**

Start and stop from the page; while active, opening the Map shows the "Not available in drive mode" toast; the broker sheet saves and `get rx.url` on serial returns the same value; the encoder and the keyboard can reach every control.

- [ ] **Step 4: Commit**

```bash
git add src/ui-touch/UITask.cpp
git commit -m "pager: Drive page to start capture and watch GPS, link and queue"
```

---

### Task 8: Linking and acceptance on live

**Files:**
- Modify: `docs/superpowers/specs/2026-11-12-rx-capture-design.md` (results)
- Create: `docs/RX_CAPTURE.md` (how to set up and use, one page)

- [ ] **Step 1: Link the Pager through coredrive-rx**

Drive mode off, BLE on. The user connects the coredrive-rx app to the Pager and is logged in to CoreScope.
Expected: the Pager's pubkey shows under Companions on the CoreScope account page.
If the app reports "firmware cannot sign": stop. Linking in the firmware is a separate design (spec, "Linking").

- [ ] **Step 2: Live run (user confirms pointing the device at the live broker)**

Set the live broker values, drive at least 2 km with a phone hotspot, then 1 km without.
Expected: rows for this pubkey in `client_receptions` for the first part during the drive, and for the second part after Wi-Fi returns; the Reach page shows hex cells along the route; `Dropped 0`.

- [ ] **Step 3: Write `docs/RX_CAPTURE.md` and update the spec**

The doc covers: what it does, the broker settings, linking through the app, what drive mode switches off, what "Sent" does and does not mean, the SD card and the 1 MB fallback. In the spec, move every item from "Not verified" that is now verified into the text with its result.

- [ ] **Step 4: Commit**

```bash
git add docs/RX_CAPTURE.md docs/superpowers/specs/2026-11-12-rx-capture-design.md
git commit -m "docs: RX capture setup and the results of the live run"
```
