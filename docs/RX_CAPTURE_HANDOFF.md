# RX capture: handoff (2026-11-13)

For whoever continues this branch on another machine (a person or a coding
agent). Read this first, then `docs/RX_CAPTURE.md` (what the feature does), the
spec `docs/superpowers/specs/2026-11-12-rx-capture-design.md` (decisions and
measurements) and the plan `docs/superpowers/plans/2026-11-12-rx-capture.md`.

## Get the code

```bash
git clone https://github.com/efiten/wadamesh.git t-pager
cd t-pager
git remote add upstream https://github.com/ALLFATHER-BV/wadamesh.git
git fetch upstream
git checkout feat/rx-capture
```

The branch is cut from `upstream/main` at `89fb0c0c`. It is fork-only: no
upstream PR is planned. Rules of the owner: explicit `git add <files>`, one commit
per logical change, no force-push, check `git log --oneline upstream/main..HEAD`
before a push, no attribution lines in commits.

## Build, test, flash

Needs PlatformIO Core (`pip install platformio`, or the VS Code extension) and a
C++ compiler for the host tests (`c++` in Git Bash, MSYS2 or WSL).

```bash
# host tests (41), last line must be "rxcapture: all passed"
bash test/rxcapture/run.sh

# firmware (SX1262 Pager). On Windows PYTHONUTF8=1 is required: without it
# PlatformIO fails with UnicodeDecodeError on the partition CSV.
PYTHONUTF8=1 pio run -e tlora_pager_sx1262_companion_radio_touch

# flash over USB. Find the port first (Device Manager, USB VID 303A PID 1001).
PYTHONUTF8=1 pio run -e tlora_pager_sx1262_companion_radio_touch -t upload --upload-port COMx
```

A full build takes 5 to 7 minutes. An upload can fail once with "The chip stopped
responding": run it again. The device uses a fork-specific partition table (two
5 MB app slots); do not use the in-firmware OTA, it installs a stock image.

Diagnostic build: add `PLATFORMIO_BUILD_FLAGS="-D RX_CAPTURE_SELFTEST=1"`. It adds
the serial command `rx test` (publishes `{"type":"TEST"}`) and a Wi-Fi and DNS
line to `rx status`. In drive mode with failing DNS that extra lookup once left
the serial console unresponsive for more than a minute; cause not investigated.

Serial console: 115200 baud, commands end with CR. `drive on`, `drive off`,
`drive status`, `rx status`, `get rx.url`, `get rx.user`,
`set rx.url|rx.user|rx.pwd <value>`.

## State of the device

- Runs the build of commit `2eae5fd1`. That build does NOT contain the review
  fixes (`2148cb1c`, `0f2b04f6`, `3340c5ad`). Flash the current branch first.
- Seen with that old build on the road (2026-11-13): the Drive page starts and
  receives with a GPS fix, but the Pager did not join the phone hotspot. That is
  the behaviour review finding C1 describes (below). Until the new build is on
  the device: switch Bluetooth off in the settings, or re-apply the Wi-Fi
  network in the settings, to make it retry. Records are queued on the SD card
  meanwhile.
- Broker URL, user and password are stored on the device. The password is not in
  this repository: it is `mqttPassword` in the coredrive-rx `config.json`.
- An SD card is inserted; the queue file opens on it (`queue on SD`).
- A full flash backup of the original firmware (wadamesh beta_103) exists only on
  the first development PC: `C:\dev\meshcore\t-pager-backup\`.

## What is done

| Commit | What | Tested on the device |
| --- | --- | --- |
| `d81b3ed7` | Drive mode frees BLE, servers, map tiles, Lua | yes (heap measured) |
| `1f12b7ac` | Record builders (`RxRecord`) | host tests |
| `ae42c8e0` | Gates (`RxGate`) | host tests |
| `8abaa82e` | File queue (`RxQueue`) | host tests |
| `86a3378d` | MQTT session over WSS (`RxTransport`) | yes: connect, 2 publishes with PUBACK, wrong CA refused |
| `cb43b4b5` | Capture wiring (`RxCapture`) | no |
| `ac0bb252` | Drive page | no |
| `cf665883` | `docs/RX_CAPTURE.md` | n/a |
| `2eae5fd1` | BLE comes back after drive mode | yes: 3 on/off cycles |
| `2148cb1c` | Review fix: records wait in memory (`RxPump`) | host tests only |
| `0f2b04f6` | Review fix: MQTT client destroyed at the retry time | no |
| `3340c5ad` | Review fixes: Wi-Fi keeps retrying in drive mode, SD gate, BLE restore check, start/stop outside the LVGL callback | no |

## What is open, in order

1. **Flash the current branch and bench-check the review fixes.** The final
   review is done (1 Critical, 6 Important, 14 Minor; all Critical and Important
   ones fixed except one that is only reduced, see the ledger lines starting with
   "Final:"). None of the fixes ran on a device. Check, with `rx status` on serial:
   - C1: associate with Wi-Fi, `drive on`, switch the access point or hotspot off
     for 30 s, switch it on. Expected: Link goes back to `connected` by itself.
     Also: boot out of range of the network, start a drive, then bring the
     hotspot up. Expected: it joins.
   - `drive off`: `[heap] drive: before restore` shows at least 51 KB free, no
     `BLE_INIT: Malloc failed`, and the phone can connect over BLE again.
   - Start and Stop from the Drive page (they now run one loop pass later).
   - With traffic and the link up: Queued stays 0 while Sent counts up (records
     wait in memory, not in the file).
2. **Capture end to end** (plan Task 6 Step 6). Needs a GNSS fix (outdoors),
   movement, GPS on in the settings, and Wi-Fi that resolves DNS. Expected: Sent
   counts up on the Drive page while moving; records for the Pager's public key
   arrive at the ingestor with the right position; an `/rf` message at most every
   15 s while moving.
3. **Queue drain**: Wi-Fi off, hear 5 packets while moving, Wi-Fi on. Expected:
   `pending 5` then `pending 0`, 5 messages, oldest first. Repeat with a power
   cycle while 5 are pending: 5 messages after reboot and `drive on`, no
   duplicates.
4. **Drive page on the screen** (plan Task 7 Step 3): layout, Start and Stop, the
   three broker buttons save (check with `get rx.url`), encoder and keyboard reach
   every control, Map tab shows "Not available in drive mode".
5. **Linking** (plan Task 8 Step 1): the live CoreScope requires linked
   companions. Connect the coredrive-rx app to the Pager over BLE once, drive mode
   off. If the app reports that the firmware cannot sign: stop, that is a separate
   design (spec, "Linking").
6. **Live run** (plan Task 8 Step 2): 2 km with a hotspot, 1 km without. Then move
   the verified items in the spec and in `docs/RX_CAPTURE.md` out of "Not
   verified".
7. Not built: `tlora_pager_lr1121_companion_radio_touch`. The T-Deck env
   (`LilyGo_TDeck_companion_radio_touch`) was last built after the Drive page
   commit, not after the review fixes.
8. Not tried: a phone reconnecting over BLE after `drive off`.
9. Known and left open: "Stop drive" or saving a broker setting while the MQTT
   client is connecting can freeze the UI for up to 10 s. The 12 deferred minor
   findings are in the ledger ("minor (deferred)").

Known environment problem, not a firmware bug as far as measured: at the first
test location the Pager's Wi-Fi was weak (RSSI -87 to -93 dBm, half of the pings
lost) and DNS lookups from the Pager failed in three consecutive boots; the owner
saw the same on the stock firmware. Test with a phone hotspot next to the device.

Confirm with the owner before flashing the device and before anything that
publishes to the live broker.

## Decisions made during implementation (the ledger)

The working ledger is git-ignored, so its decisions are copied in
`docs/superpowers/plans/2026-11-12-rx-capture-ledger.md`. Each "Ruling" line is a
deviation from the plan or a judgment call, with what it costs if it is wrong.
To continue with the superpowers executing-plans skill, copy that file to
`.superpowers/sdd/2026-11-12-rx-capture/progress.md`.
