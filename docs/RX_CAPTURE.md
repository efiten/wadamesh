# RX capture (drive mode)

Fork-only feature for the LilyGo T-LoRa Pager. While drive mode is on, the Pager
records every MeshCore packet its radio hears, tags it with the GNSS position and
publishes it over MQTT to a CoreScope ingestor, the same way the coredrive-rx app
does with a companion radio. No phone is needed during the drive, only Wi-Fi (a
hotspot) for live publishing.

Status: built for `tlora_pager_sx1262_companion_radio_touch`. The broker session
(WSS, certificate check, QoS 1 publish) was tested on a device. Capture, the file
queue and the Drive page were not yet tested on a device. See the "Not verified"
list at the end.

## What it sends

- Topic `meshcore/client/<pubkey>/packets`: one record per received packet, with
  the raw packet as hex, SNR, RSSI, a UTC timestamp and the position
  (`acc_m` = HDOP x 5 m). Only while there is a fix, the clock is set and the
  device is moving (it counts as parked after 5 minutes within 75 m).
- Topic `meshcore/client/<pubkey>/rf`: an RF sample (noise floor, last RSSI and
  SNR, airtime and packet counters, battery) every 15 s or 100 m while moving,
  every 5 minutes while parked.
- On the air: a zero-hop node-discover request at most every 30 s, and not within
  15 s after other traffic was heard. Nothing else is transmitted by this feature.

MQTT 3.1.1, client id = the node's public key (64 hex characters), keepalive 60 s,
QoS 1. A record leaves the device's memory or queue only after the broker's PUBACK.

## Setup

1. GPS must be on in the settings. Drive mode does not switch it on.
2. Broker settings, on the Drive page (buttons URL, User, Password) or over serial:

   ```
   set rx.url wss://<host>:<port>/<path>
   set rx.user <user>
   set rx.pwd <password>
   ```

   The URL must start with `wss://`. The broker certificate is checked against
   ISRG Root X1, ISRG Root X2 and Root YE (Let's Encrypt). A broker with a
   certificate from another CA is refused.
3. If the CoreScope instance requires linked companions: connect the coredrive-rx
   app to the Pager over BLE once (drive mode off) while logged in to CoreScope.

## Use

Apps, Drive, "Start drive". Or over serial: `drive on`, `drive off`,
`drive status`, `rx status`.

| Row | Meaning |
| --- | --- |
| GPS | `fix N m`, `no fix`, or `parked` |
| Link | `connected`, `connecting`, `waiting for Wi-Fi`, `failed`, `not set up` |
| Sent | Records the broker acknowledged (PUBACK) |
| Queued | Records in the file queue, waiting for a session |
| Dropped | Records lost: ring full, queue full, or a write failed |
| Last | SNR of the last packet heard |
| Store | Where the queue file is |

"Sent" means the broker acknowledged the publish. It does not mean the record was
stored: the device cannot see what the broker's ACL or the ingestor does with it
(for example an unlinked companion). Check the first run on the CoreScope side.

## What drive mode switches off

BLE, the TCP and WebSocket companion servers, map tiles, Lua apps and their audio.
This frees the internal RAM the TLS session needs (about 4 KB free normally, about
54 KB in drive mode). The phone app cannot reach the Pager during a drive. A reboot
always comes back with drive mode off.

## The queue

Without a broker session, records go to `/rxcap/queue.jsonl`:

- on the SD card when one is mounted at the moment the drive starts, up to 64 MB;
- otherwise on internal flash, up to 1 MB (the record of a maximum-size packet of
  255 bytes is about 730 bytes).

When the queue is full, new records are dropped and counted. Queued records are
sent oldest first when the session is back, also after a reboot (start a drive
again). After a power cut up to 9 records that were already acknowledged can be
sent a second time.

## Firmware updates

The in-firmware OTA installs an official wadamesh image, which does not contain
this feature and uses the stock partition layout. Flash this fork over USB.

## Not verified

- Capture end to end on a device: records arriving at the ingestor with the right
  position, the queue draining after Wi-Fi returns and after a power cycle.
- The Drive page on the device (layout, encoder and keyboard reach).
- Linking through the coredrive-rx app (the firmware answers the signing commands;
  not tried).
- A phone reconnecting over BLE after `drive off` (the BLE stack and the WebSocket
  server do come back; three cycles tested).
- The `tlora_pager_lr1121_companion_radio_touch` env was not built.
