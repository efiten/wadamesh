# RX capture on the T-LoRa Pager: design

Status: proposal, not approved. Branch `feat/rx-capture` (from `upstream/main` at `89fb0c0c`).

## Intent

A LilyGo T-LoRa Pager running this firmware reports RX coverage to CoreScope by itself, with no
phone. Today that takes coredrive-rx (a phone app) plus a companion radio over BLE. The Pager has
the radio, a GNSS receiver and Wi-Fi in one device.

## Goals

- Publish every received packet with SNR, RSSI and the Pager's own position to
  `meshcore/client/<pubkey>/packets`, in the payload coredrive-rx sends. The CoreScope ingestor
  and the EMQX ACL stay unchanged.
- Publish RF samples to `meshcore/client/<pubkey>/rf`.
- Send zero-hop node-discover requests so nodes in direct range answer.
- With Wi-Fi and a broker session: publish in real time. Without: store, and drain on reconnect
  while new records keep going out live.
- A "drive mode" that switches off what a drive does not need, to free internal RAM.

## Non-goals (v1)

- Region requests (`/regions`), node names (`/names`), uplink pings (`/uplink`).
- Linking code in the firmware. The live CoreScope has `requireLinkedCompanion` on (user,
  2026-11-12), so the Pager must be linked, but that is done once with the existing coredrive-rx
  app: see "Linking" below.
- Other boards. The module is compiled only for the two `tlora_pager_*` envs.
- On-device attribution of who was heard. The server derives it from `raw`
  (`corescope-rx/src/capture.js`, comment on the record shape).

## Anti-goals

- Changing the `/packets` or `/rf` payload contract.
- Losing a record that was captured with a position fix.
- A drive mode that leaves Bluetooth off after it ends. `disableBle()` persists the off state
  (`MultiTransportCompanionInterface.cpp:344`), so drive mode must not call it as is.

## Measured on the device (2026-11-12, wadamesh beta_103 on meshcomod v1.18.0.1)

From the boot log on USB serial:

| Point | Internal free | Largest block | PSRAM free |
|---|---|---|---|
| `[MEM] boot int` (before the UI is built) | 14756 B of 235600 B | 7668 B | 7812139 B |
| `[heap] after UI init` | 9708 B | 7668 B | 6872859 B |

- `[BOOT] SD: no card detected`. The device had no micro-SD card at that boot.
- `[WS] listening port=8765 (plain)`: the WebSocket companion server was running.
- Not captured: whether BLE and Wi-Fi were up at that boot. The log lines between
  `[BOOT] mesh ok` and `[MEM] boot int` were lost.

Thresholds already in the code:

- An HTTPS request is refused below 46 KB free internal heap (`src/ui-touch/LuaAppHost.cpp:3201`).
- A cold NimBLE start is refused below 50 KB free or a 20 KB largest block
  (`src/helpers/esp32/MultiTransportCompanionInterface.cpp:272`).

So at the measured 9.7 KB a TLS session does not fit. Whether it fits in drive mode is the
first thing the plan measures.

## Approaches

1. **New `RxCapture` module with its own MQTT session (chosen).** Hooks `MyMesh::logRxRaw()`
   (`src/MyMesh.cpp:2336`), owns queue, gates and transport. `MqttBridge` is untouched.
   Costs a second MQTT client in the binary. Keeps the diff against upstream small and in new
   files, which matters for a fork that tracks a fast-moving `main`.
2. **Extend `MqttBridge`.** One client, but that class is plain MQTT on PubSubClient at QoS 0
   with its own topics and privacy model (`src/helpers/esp32/MqttBridge.h`). QoS 1 with
   acknowledgement tracking, a second topic tree and optional TLS would rewrite most of it and
   conflict with every upstream change to it.
3. **Rebase upstream's `mqtt-observer` branch (draft PR #520).** It has raw-packet publishing,
   an offline queue and WSS with JWT. It is 759 commits behind `main`, held for discussion,
   carries 39 network profiles this feature does not use, and its records have no position.

Choice 1 is the author's call, not yet confirmed by the user.

## Design

### Drive mode

Entered from a new screen, left the same way or by reboot. On entry, in this order:

1. Suspend BLE without persisting: `NimBLEDevice::deinit(true)` after detaching the server
   callbacks, remembering the previous state. The OTA path already does exactly this
   (`MultiTransportCompanionInterface.cpp:479-492`, restore at `:518`).
2. Stop the TCP (5000) and WebSocket (8765) companion servers, as the OTA path does (`:466-478`).
On exit everything is restored to its previous state.

3. Free the map tiles (`freeMapTiles()`, `src/ui-touch/UITask.cpp:39722`), stop audio
   (`luaHostAudioStop()`, `UITask.cpp:75936`) and close any running Lua app. None of these is
   needed during a drive (user, 2026-11-12). Their saving in internal RAM is not measured, and a
   runtime "close the Lua app" entry point was not found yet: the builder locates or adds it.
4. While drive mode is on, the map, Lua apps and audio cannot be opened.

The heap is logged before and after each step.

### Step 0: measure, then fix the transport

Build drive mode first and read the heap log on the device.

- At least 46 KB free internal with Wi-Fi associated: use **WSS on 8084**, the same listener
  coredrive-rx uses, with the server certificate verified. coredrive-rx verifies it too: the
  browser or WebView checks the chain against the system trust store. The broker's certificate
  is issued by Let's Encrypt (issuer `CN=YE2`, read 2026-11-12), so the firmware bundles the
  Let's Encrypt root certificates. Which root `YE2` chains to was not checked.
- Less: use **plain MQTT on 1883**. The broker password then crosses every Wi-Fi network the
  Pager joins unencrypted, and the EMQX listener must be opened.

### Capture

`logRxRaw()` hands `(snr, rssi, raw, len)` to the module. A record is made when drive mode is on,
a position fix exists and the device is not stationary. No fix, no record: a reception without a
position is not a map point.

`/packets` payload, identical to `Publisher.buildPayload` (`corescope-rx/src/publisher.js:118`):

```json
{ "origin_id": "<pubkey hex, lowercase>", "origin": "<node name>", "timestamp": "<ISO 8601 UTC>",
  "type": "PACKET", "direction": "rx", "raw": "<hex>", "SNR": 0.0, "RSSI": 0,
  "gps": { "lat": 0.0, "lon": 0.0, "acc_m": 0.0 } }
```

`acc_m` comes from the GNSS HDOP. The conversion factor is the builder's to choose and document.

`/rf` payload, identical to `Publisher.buildRfPayload` (`publisher.js:133`), filled from the
firmware's own counters instead of three BLE requests. One sample every 100 m while moving, at
most 15 s apart (coredrive-rx commit `436f859`).

### Discover

Zero-hop node-discover at most every 30 s, held back 15 s after any overheard traffic, suspended
while stationary (`corescope-rx/docs/user-guide.md:57`). This is the only part that transmits.

### Queue and publishing

- MQTT 3.1.1, clientId = pubkey, QoS 1. A record leaves the queue only after its PUBACK.
- Connected: records go straight out; a small PSRAM buffer holds the unacknowledged ones.
- Not connected: records are appended to a file, one JSON line each. On reconnect the file drains
  oldest first while live records keep flowing.
- Storage: the SD card when one is mounted. With no card, a file on the internal data partition
  capped at 1 MB; when full, new records are dropped and counted on the status screen. The cap
  and the no-card fallback are the author's call.

### Linking

The firmware already answers the companion sign commands (`CMD_SIGN_START/DATA/FINISH`,
`src/MyMesh.cpp:5356-5378`), which is what coredrive-rx uses to link a companion
(`corescope-rx/src/companionsign.js`). So the Pager is linked once by connecting the coredrive-rx
app to it over BLE while logged in to CoreScope. The link is stored server-side per pubkey; the
firmware needs no account code. Not tested: that this firmware's sign exchange is accepted by
the app. If it is not, linking in the firmware (HTTPS login, challenge, sign, post) becomes its
own follow-up design.

### Settings and screen

- Broker URL, username and password are stored in the existing prefs store and set from the
  drive screen and from the serial `set` commands (`MyMesh::checkCLIRescueCmd`).
- One screen: start/stop, GPS fix and accuracy, link state, queued count, dropped count, records
  sent, last SNR.

### Tests

- Host tests under `test/` (the repo's existing fake-based pattern) for the payload builders, the
  gates and the queue (append, drain order, acknowledgement, cap).
- On the Pager against the staging broker: a record appears in `client_receptions`; Wi-Fi off and
  on again drains the file; drive mode off brings BLE and both servers back.

## Left to the builder

- Module file layout and class names.
- The MQTT client library, once step 0 has fixed TLS or plain.
- The stationary detector (GNSS speed, position delta, or the Pager's IMU).

## Not verified

- That the SX1262 env is the right one for this unit (the board is also sold with an LR1121).
- That `origin_id` must be lowercase. coredrive-rx lowercases the pubkey (`app.js:1875`); the
  ingestor's handling of case was not read.
- That the GNSS provider exposes HDOP.
- No code was built or flashed. The heap figures come from the stock firmware already on the
  device.

## Drive mode measured (2026-11-13, fork build with drive mode, SX1262 unit)

Wi-Fi associated, BLE enabled, no SD card, no Lua app open, map not opened. Internal heap in
bytes, from the `[heap]` lines on USB serial:

| Point | Free | Largest block |
|---|---|---|
| Boot, after Wi-Fi association | 74560 | 65524 |
| Boot, after BLE init | 25272 | 17396 |
| Boot, after UI init | 9772 | 7668 |
| Idle, about 45 s after boot (`drive status`) | 3956 | 3060 |
| `drive on`: after BLE and both servers are released | 49416 | 23540 |
| `drive on`: after map tiles and Lua are freed | 53888 | 47092 |
| `drive off` | 4256 | 3060 |

- Releasing BLE and the two companion servers returns about 45 KB. NimBLE alone costs about
  49 KB at boot (74560 to 25272).
- The map-tile and Lua step returned 4472 bytes in this run, with no map or Lua app open. In a
  second run it returned nothing.
- After `drive off` the heap is back at the idle level and `[WS] listening port=8765` is logged
  again. That the phone can reconnect over BLE was not checked.
- A reboot while drive mode was on came back with `drive: off`, `BLE stack ready (enabled=1
  wifi=1)` and the WebSocket server listening.

**Transport decision:** 53888 bytes free is above the 47104-byte rule, so `RX_TLS=1`: WSS on
8084 with certificate verification. The margin is 6784 bytes. Task 5 measures the heap during a
live TLS session; if the session does not hold, the fallback is plain MQTT on 1883 and needs the
user's decision.

The device's radio is the SX1262 (the flash backup contains `wadamesh-tlora-pager-sx1262`).

## Transport measured (2026-11-12 23:52 UTC, Task 5)

The 46 KB rule was not enough. With mbedTLS allocating from internal RAM (the SDK default,
`CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC`, with fixed 16 KB record buffers) the first handshake failed
with `-0x0010` (allocation failed), the heap's low-water mark was 24 bytes, and every later
attempt failed in `mbedtls_ssl_setup` with `-0x7F00`.

Change: while drive mode is on, mbedTLS allocates from PSRAM
(`mbedtls_platform_set_calloc_free`, in `DriveMode.cpp`), and goes back to internal RAM on leave.
This is a process-wide switch, so any other TLS user in the firmware is also on PSRAM during
drive mode.

Results with that change, against `wss://<broker>:8084/ws` with the publish-only account:

- Connected with the certificate verified against the embedded roots. Chain read from the
  broker: leaf, Let's Encrypt YE2, Root YE, cross-signed by ISRG Root X2, cross-signed by ISRG
  Root X1. Embedded: Root YE, ISRG Root X2, ISRG Root X1.
- Internal heap with the session up: 44456 bytes free, largest block 38900.
- Two QoS 1 publishes to `meshcore/client/<pubkey>/packets` with payload `{"type":"TEST"}`: two
  PUBACKs on the device (`sent 2`). The broker's REST API showed one client with this username,
  a 64-character client id, MQTT 3.1.1, keepalive 60, `recv_msg.qos1` 2, dropped 0, connected at
  the time drive mode was switched on. What the ingestor did with the two test messages was not
  checked.
- A host with a certificate from another CA (dummy credentials set first) was refused:
  `mbedtls_ssl_handshake returned -0x2700` (certificate verify failed), nothing published.
- Flash: 4092001 of 5242880 bytes (78.0 percent); the transport added 42144 bytes.

Observed and not explained:

- The test location has weak Wi-Fi (RSSI -93 dBm). Two of the five connection attempts in this
  session failed before TLS, with a DNS failure (esp-tls error 32769) and a connection timeout
  (32774). The session retries every 10 s.
- After the last two `drive off` commands no `[WS] listening port=8765` line was seen within
  25 s, and port 8765 did not answer from the PC. In Task 1 that line appeared directly after
  `drive off`. Whether the WebSocket server and BLE come back reliably after drive mode is open.

## Flash space (found 2026-11-12, during Task 1)

The Pager build with drive mode used 4049857 of 4063232 bytes of the 3.875 MB app slot (99.7
percent, 13375 bytes free). The size of the build without drive mode was not measured. The
"~2.55 MB" figure in the stock partition file's comment is out of date.

Decision (user, 2026-11-12): larger app slots. `partitions_tlora_pager_rxcapture.csv` gives both
slots 5 MB and shrinks the internal tiles partition from 4.75 MB to 2.5 MB. `spiffs` and
`coredump` keep their offsets, so user data survives; the internal tile cache is lost once. The
same build then uses 77.2 percent of a slot. Going to or from this table needs a full USB flash.

## Decided

- `requireLinkedCompanion` is on: link once through the coredrive-rx app (see "Linking").
- TLS, if it fits, verifies the broker certificate.
- The work stays in the fork `efiten/wadamesh`. No pull request to upstream is planned.
