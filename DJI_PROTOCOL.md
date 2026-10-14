# DJI Osmo Action 5 Pro: BLE protocol notes

Research notes for connecting the ESP32 bridge to the camera as a BLE central. Status as of 2026-10-14.
Items marked **tested** were seen on our Action 5 Pro with `src/dji_test.cpp` (2026-10-14).
Items marked **unverified** come from no source, or from one source only.

The camera speaks DJI's open "R SDK" BLE protocol. It gives everything the bridge needs:

| Need | Where |
|---|---|
| Battery % | Status push `1D02`, payload byte 37 |
| Recording state | Status push `1D02`, payload byte 1 (with byte 0 = mode) |
| Start / stop recording | Command `1D03` |

Pairing is part of the protocol, not BLE bonding. The user taps **Accept** on the camera once.
Reconnections are silent after that.

The CRC parameters below were checked against DJI's example frames and against real
Action 5 Pro frames posted in issue #5. All matched.

Main source: [dji-sdk/Osmo-GPS-Controller-Demo](https://github.com/dji-sdk/Osmo-GPS-Controller-Demo),
commit `92fe23e` (2025-11-17). Paths below are relative to that repository.

## Support

- The README and `docs/protocol.md` list "Osmo 360 or DJI Osmo Action 6 / 5 Pro / 4".
- Camera device ID for the Action 5 Pro: `0xFF44` (`docs/protocol_data_segment.md`).
- A DJI maintainer confirmed Action 5 Pro support in issue #9, and advised updating the camera
  to the latest firmware. No minimum firmware version is documented.
- [Issue #5](https://github.com/dji-sdk/Osmo-GPS-Controller-Demo/issues/5) has a real Action 5 Pro
  handshake log.
- Already working on a classic ESP32 with an Action 5 Pro:
  [theserialhobbyist/M5StickCPlus2_Remote_For_DJI_Osmo](https://github.com/theserialhobbyist/M5StickCPlus2_Remote_For_DJI_Osmo) (ESP-IDF)
  and [rhoenschrat/DJI-Remote](https://github.com/rhoenschrat/DJI-Remote) (M5Stack Basic).

## Frame format

All fields are little-endian. Sources: `docs/protocol.md`, `protocol/dji_protocol_parser.c`.

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | SOF `0xAA` |
| 1–2 | 2 | Bits 9..0: total frame length including CRC32. Bits 15..10: version 0 |
| 3 | 1 | CmdType. Bits 4..0: reply wanted (0 none, 1 optional, 2+ required). Bit 5: 1 = this is a response |
| 4 | 1 | ENC = 0 |
| 5–7 | 3 | Reserved. The camera's own pushes have non-zero bytes here, so do not check them |
| 8–9 | 2 | SEQ |
| 10–11 | 2 | CRC16 over bytes 0–9 |
| 12 | 1 | CmdSet |
| 13 | 1 | CmdID |
| 14… | n | Payload |
| end−4 | 4 | CRC32 over byte 0 up to the end of the payload |

Minimum frame length is 16 bytes.

**CRC16** (`utils/crc/custom_crc16.h`):
- Polynomial 0x8005, reflected (table form 0xA001).
- Input and output reflected.
- **Init 0x3AA3**, no final XOR.

**CRC32** (`utils/crc/custom_crc32.h`):
- Polynomial 0x04C11DB7, reflected (0xEDB88320).
- Input and output reflected.
- **Init 0x00003AA3**, no final XOR.
- This is **not** standard CRC-32, which uses init 0xFFFFFFFF and a final XOR.

**SEQ:**
- The sender picks the SEQ. A response must echo the request's SEQ.
- The demo increments a global counter for each command it sends.
- When answering a frame the camera started (its pairing approval, `1D02`/`1D06` pushes),
  reuse the camera's SEQ.
- The demo had SEQ byte order wrong before 2025-07-02, so older logs show it swapped.

Check frame from the docs (mode switch to Hyperlapse):

```
AA 1B 00 01 00 00 00 00 05 00 57 EE 1D 04 00 00 33 FF 0A 01 47 39 36 F4 FA E1 D0
```

**Other traffic:** the camera also sends DUML frames (SOF `0x55`, different CRCs) on the same
characteristic. DJI says to ignore anything that does not start with `0xAA` (`docs/Q&A.md` #13).

## Transport rules

- GATT: service `0xFFF0`, notify characteristic `0xFFF4` (camera → remote),
  write characteristic `0xFFF5` (remote → camera) (`docs/Q&A.md` §1).
- **One frame per ATT write.** The camera does not accept a frame split into 20-byte writes
  (issue #14, and a Garmin forum thread where `1D03` got no reply). The connection request alone
  is 51 bytes, so the MTU must be at least 54.
- **One frame per notification.** At MTU 23, frames from the camera arrive truncated (issue #14).
  The largest R SDK frame seen is the 114-byte version reply. The demo sets MTU 500, and the
  Action 5 Pro agreed to 500 in issue #5.
- Write type: Write Request for CmdType 1/2, Write Command for CmdType 0x00/0x20
  (`ble/ble.c`, `logic/command_logic.c`).
- Arduino-ESP32: `BLEClient` sends an MTU request on connect. Call `BLEDevice::setMTU(500)`
  before connecting.

**Arduino-ESP32 2.0.17 quirks (tested).** These are probably the "quirks" the M5Stick author meant:
- On the first connection after boot, `BLEClient` skips its MTU request: its connect-event handler
  compares against a connection ID that is only set later. The link stays at MTU 23 and every
  camera frame arrives cut off at 20 bytes. Fix: call `client->setMTU(500)` after `connect()`.
- `BLERemoteCharacteristic::writeValue()` blocks until the write's completion event, even for
  Write Command. That deadlocks in a notify callback, and it hung in `loop()` while the camera was
  sending notifications fast. Fix: call `esp_ble_gattc_write_char()` directly; it only queues.

## Connecting and pairing

Sources: `logic/connect_logic.c`, `docs/protocol_data_segment.md`, the issue #5 logs.

1. **Scan** (active). Accept advertisements whose manufacturer data has byte 0 = `0xAA`,
   byte 1 = `0x08` and byte 4 = `0xFA` (`ble/ble.c`).
   - Bytes 2–3 = `15 00` means Action 5 Pro (model 0x0015).
   - Bytes 5–10 are the camera MAC.
   - Example: `AA 08 15 00 FA 60 60 1F C7 50 96 00` → MAC 60:60:1F:C7:50:96.
2. **Connect** and request a large MTU (see Transport rules).
3. **Discover** service `0xFFF0`.
4. **Enable notifications** on `0xFFF4` (write `01 00` to its CCCD). The demo then waits 2 s.
5. **Send a connection request:** CmdSet `0x00`, CmdID `0x19`, CmdType `0x02`. 33-byte payload,
   51-byte frame:

   | Offset | Size | Field | Value |
   |---|---|---|---|
   | 0 | 4 | device_id (u32) | The remote's own ID. Keep it constant |
   | 4 | 1 | mac_len | 6 |
   | 5 | 16 | mac | 6 bytes, then zero padding. Keep it constant |
   | 21 | 4 | fw_version | 0 (stops firmware-update prompts) |
   | 25 | 1 | conidx | 0 |
   | 26 | 1 | verify_mode | 1 = force the code pop-up (first pairing); 0 = camera decides from its history |
   | 27 | 2 | verify_data | Random 0–9999 (the code shown on screen) |
   | 29 | 4 | reserved | 0 |

   - The demo always uses verify_mode 0, device_id 0x12345678 and a fake MAC 38:34:56:78:9A:BC.
   - DJI-Remote and the M5Stick project use verify_mode 1 for the first pairing and 0 for
     reconnects, with a random device_id stored once in NVS.
   - A ready-made frame is in `test/connect_cmd_frame_builder/connect_cmd_frame.txt`:

   ```
   AA330002000000000100982F00197856341206383456789ABC00000000000000000000000000000000FE1F00000000796C667C
   ```

6. **The camera replies** with a response frame (CmdType `0x20`). Payload: device_id, ret_code,
   reserved. Action 5 Pro capture (device_id 0xFF44, ret_code 0):

   ```
   AA 1B 00 20 00 00 00 00 00 01 A4 7C 00 19 44 FF 00 00 00 00 00 00 00 42 D8 ED 0F
   ```

   The camera may send this response or its own command (step 8) first. Handle either order.
   **Tested:** our camera sent exactly this payload.
7. **First pairing only:** the camera shows the code. The user taps Accept.
8. **The camera sends its own `0x0019` command** (CmdType 0x02) with verify_mode = 2 and
   verify_data = 0 (accepted) or 1 (rejected). The demo waits up to 30 s for it. Action 5 Pro capture:

   ```
   AA 33 00 02 00 00 00 00 01 00 98 2F 00 19 44 FF 00 00 | (mac_len 0, 16×00, fw 0, conidx 0) 02 00 00 00 00 00 00 | E1 EA 9C 15
   ```

9. **Acknowledge it** with a `0x0019` response frame (CmdType 0x20) that uses the **camera's SEQ**.
   Payload: own device_id, ret_code 0, reserved[0] = camera number (0 for a single camera).
   If the camera rejected the pairing, disconnect and do not retry during that scan.
   **Tested:** the camera repeats its command about 50 times a second, with a new SEQ each time,
   until this response arrives. A Write Command with a 9-byte payload (all zero except device_id)
   is enough.
10. Optionally query the version (CmdSet 0x00, CmdID 0x00). Then subscribe to status (`1D05`, below).

**Bonding:**
- Not required. The demo never encrypts the link and uses no-auth reads and writes, and the
  issue #5 logs show a full handshake that way.
- **Unverified:** whether the camera ever sends an SMP security request.

**Reconnecting:**
- With verify_mode 0 and the same identity, no pop-up appears while the camera keeps its
  pairing record.
- To clear the record on the camera: Settings → Wireless Connection → reset connection (Q&A #12).
- A DJI maintainer (issue #5) said that changing the remote's MAC (`g_mac_addr`) also forces a new
  pairing. Keep device_id and mac constant.
- **Unverified:** whether the camera also checks the BLE link-layer address.

## Status push `1D02`

**Subscribe** with `1D05`. Payload: push_mode, push_freq, reserved[4].
- push_mode 3 = periodic, plus a push whenever the status changes.
- push_freq must be 20 (fixed 2 Hz).
- The demo sends it with CmdType 0x00 (`logic/status_logic.c`).
- The camera acknowledges with a 0x20 frame `1D 05 00 00 00 00 00` (issue #5; **tested**).

Computed subscribe frame:

```
AA 18 00 00 00 00 00 00 02 00 51 EE 1D 05 03 14 00 00 00 00 30 FC E7 44
```

**`1D02` payload** (camera → remote, CmdType 0x01 observed; the demo does not acknowledge it).
For full-frame offsets, add 14. Struct in `protocol/dji_protocol_data_structures.h`.

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 1 | camera_mode | 0x00 SloMo, 0x01 Video, 0x02 Timelapse, 0x05 Photo, 0x0A Hyperlapse, 0x28 SuperNight, …; values above 0x34 use `1D06` |
| 1 | 1 | camera_status | 0x00 screen off, 0x01 live view / idle, 0x02 playback, **0x03 photo or recording**, **0x05 pre-recording** |
| 2 | 1 | resolution | |
| 3 | 1 | fps_idx | |
| 4 | 1 | EIS | |
| 5–6 | 2 | record_time (u16) | seconds |
| 7 | 1 | fov | |
| 8 | 1 | photo_ratio | |
| 9–10 | 2 | countdown | |
| 11–12 | 2 | tl_interval | |
| 13–14 | 2 | tl_duration | |
| 15–18 | 4 | remain_capacity | MB |
| 19–22 | 4 | remain_photo | |
| 23–26 | 4 | remain_time | seconds |
| 27 | 1 | user_mode | |
| 28 | 1 | power_mode | 0 normal, 3 sleep |
| 29 | 1 | next_mode_flag | |
| 30 | 1 | temp_over | 0–3 |
| 31–34 | 4 | photo_countdown_ms | |
| 35–36 | 2 | loop_record | |
| 37 | 1 | **battery** | **percent, 0–100** |

- **Recording test:** camera_status is 0x03 or 0x05 (`logic/status_logic.c`). 0x03 also covers
  taking a photo, so also check that camera_mode is a video mode, or that record_time is counting up.
- **Tested** (payload at least 38 bytes, battery at byte 37 correct): Video is mode 0x01, SuperNight
  is 0x28, idle is status 0x01. While recording, status is 0x03 and record_time counts up once a
  second. After a stop, status stays 0x03 for about 1 s (record_time drops to 0) and then returns to 0x01.
- **Pre-rec (tested):** status 0x05 means the pre-recording buffer is armed. Entering it shows
  0x03 with record_time 0 for one push, then 0x05. record_time counts the buffered seconds up to the
  configured length (60 s here) and stays there. The shutter switches 0x05 straight to 0x03, and
  record_time carries on from the buffer (60, 61, …). On stop the camera goes 0x03 (time 0) → 0x01
  → 0x03 (time 0) → 0x05, back to pre-rec. So "recording" = 0x03 with record_time > 0.
- **Payload length:** older Action 4 firmware sent only 31 payload bytes (issue #5). Read byte 37
  only when the payload is at least 38 bytes.
- The demo parses the battery field but never uses it.
- **Unverified:** the actual push rate.

Real Action 5 Pro frame from issue #5 (mode 0x05 Photo, status 0x03, battery 0x43 = 67 %):

```
AA 38 00 01 00 52 2A 00 E5 D2 A2 96 1D 02 | 05 03 03 01 00 00 00 00 01 00 00 00 00 00 00 A8 DB 01 00 B2 4C 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 43 | 79 B7 7F D9
```

**`1D06` (new-style status push):** the mode name and parameters as ASCII. No battery or
recording information. On new firmware it is sent once after connecting and when the mode changes
(issue #8).

## Record start / stop: `1D03`

CmdSet 0x1D, CmdID 0x03, CmdType 0x01 (as in the demo), 9-byte payload
(`docs/protocol_data_segment.md`, `logic/command_logic.c`):

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | device_id (u32 LE) | See below |
| 4 | 1 | record_ctrl | 0 = start, 1 = stop |
| 5 | 4 | reserved | 0 |

- device_id is inconsistent between sources: the demo sends 0x33FF0000, AeroCam sends
  0xFF440000 (camera ID << 16), and the docs' example uses `00 00 33 FF`. This suggests the camera
  does not check it. **Unverified.**
- Reply (per the docs): a response frame with the same SEQ. Payload: ret_code (0 ok, 1 parse error,
  2 execution failed, 0xFF undefined) + reserved[4].
- **Tested:** device_id `00 00 FF 33` (the demo's) works. The reply is a response frame with the
  same SEQ, non-zero reserved header bytes and a **1-byte payload** (no reserved[4]). Start and stop
  both return ret_code 0x00. Undocumented codes:
  - **0xD6:** start with no SD card in the camera.
  - **0xD9:** stop while not recording (seen with pre-rec armed). Nothing changes; pre-rec stays armed.

  ```
  ok:    AA 13 00 20 00 41 40 31 08 00 7D BC 1D 03 00 AE 8F 26 B1
  no SD: AA 13 00 20 00 41 40 31 03 00 7A 8C 1D 03 D6 04 88 9A 9E
  ```
- **With pre-rec armed (tested):** start works like the shutter button. It keeps the buffer
  (record_time carries on from 60) and returns 0x00. Stop during that recording returns 0x00, and
  the camera goes back to pre-rec. So the bridge can pass start and stop through unchanged.

Frames computed with the demo's CRC code (SEQ 1; not captured from a camera):

```
start: AA 1B 00 01 00 00 00 00 01 00 55 2E 1D 03 00 00 FF 33 00 00 00 00 00 27 3E C9 A3
stop:  AA 1B 00 01 00 00 00 00 01 00 55 2E 1D 03 00 00 FF 33 01 00 00 00 00 97 17 A9 9E
```

**Alternative recommended by DJI** (Q&A #11): Key Report, CmdSet 0x00 / CmdID 0x11,
key_code 0x01 (record button), mode 0x01, key_value 0x0000 (short press). It works like pressing
the shutter button, so it **toggles**. Computed frame:

```
AA 16 00 01 00 00 00 00 03 00 0C DE 00 11 01 01 00 00 9A FC D9 59
```

Notes:
- The demo starts recording when mode == Photo or status == 0x01 (`logic/key_logic.c`).
  **Unverified:** whether a `1D03` start in Photo mode takes a photo. Mode switch `1D04` with
  mode 0x01 selects Video first.
- DUML camera-control commands (CmdSet 0x02/0x02) get no reply on similar cameras (osmosis notes).
  Do not use the DUML path for recording.

**Key report values** (`docs/protocol_data_segment.md`): key_code 0x01 Record, 0x02 QS (Quick Switch),
0x03 SnapShot (from sleep). mode 0x00 = press/release (key_value 0 press, 1 release); mode 0x01 =
event (key_value 0 short, 1 long, 2 double, 3 triple, 4 quadruple press).

## Pre-Rec on / off: QS key report (tested)

The R SDK has no Pre-Rec command, and DJI's UI spec says the camera starts Pre-Rec itself. But since
firmware 01.03.02.10 the camera's QS button can be set to start and end Pre-Rec (Control Center →
Quick Switch Button Settings → Start/End Pre-Rec), and a QS key report acts as a short press of that
button (Q&A #10).

- **Tested:** with QS set that way, a QS short press toggles Pre-Rec on and off. In 1D02, status
  0x01 → 0x05 and back:

  ```
  key report QS, short press: CmdSet 0x00, CmdID 0x11, CmdType 0x01, payload 02 01 00 00
  ```

- It's a toggle, so check status 0x05 before pressing. It also depends on the camera setting: with
  QS left at "Switch Between Modes" it would switch modes instead (issue #25, Osmo 360).
- Pre-Rec works only in Video and SuperNight modes (user manual). On the camera it is enabled from the
  top-left icon in those modes, where Pre-Rec and Loop Recording share one control.

**Not needed, for reference** (research only, untested):
- DUML has a camera-settings command, CmdSet 0x02 / CmdID 0x8E (GET `00 <count> <pid u16>`,
  SET `01 01 <pid u16> <len> <value>`), used over BLE by Moblin and osmosis after a separate
  Mimo-style pairing (0x07/0x45). A decompile of Mimo 2.12.1 for the Action 5 Pro
  ([RB14/dji-vcam](https://github.com/RB14/dji-vcam/blob/main/docs/camera-controls.md)) shows it
  ending Pre-Rec with SET pid 0x003D = 0. No "start Pre-Rec" setter was found. Frame format and CRCs:
  [osmosis MEDIA_PROTOCOL.md](https://github.com/KonradIT/osmosis/blob/main/MEDIA_PROTOCOL.md).

## Other battery sources (not needed)

- **DUML push** (SOF 0x55) on the same `0xFFF4`: CmdSet 0x0D / CmdID 0x02, about 1 Hz, percent
  at payload offset 20. Sources: [osmosis](https://github.com/KonradIT/osmosis) `MEDIA_PROTOCOL.md`,
  [node-osmo](https://github.com/datagutt/node-osmo) `src/device.ts`,
  [djictl](https://github.com/xaionaro-go/djictl) `pkg/duml/battery.go`.
  Issue #14 shows these frames arriving even before the R SDK handshake.
  **Unverified:** whether they arrive with no pairing at all.
- **Standard Battery Service:** the Action 5 Pro also advertises Battery Service and HID next to
  `0xFFF0` (`docs/images/oa5pro_bluetooth_scan_example.png`). **Unverified:** whether it can be read
  without bonding.

## One controller, sleep and wake-up

- **One controller at a time.** DJI's getting-started guide: "After the BLE layer establishes a
  connection, the camera's communication channel is actually occupied, and other devices will no
  longer be able to connect via BLE." **Unverified:** whether this also locks out the Mimo app.
- **Sleep** (long press on the power button, or command `0x001A` with power_mode 3) keeps the BLE
  link up. `1D02` then shows power_mode 3. Do not send commands while the camera sleeps.
  On wake-up the link drops briefly and the remote must reconnect (`docs/Q&A.md` #2).
  - `001A`: payload 1 byte power_mode (0 normal, 3 sleep); the response is a 1-byte ret_code.
    osmoControl notes the ack may not arrive before the camera sleeps.
  - DJI: the camera may still send a few status frames while falling asleep, then nothing.
  - Nothing wakes a sleeping camera over the existing link. vcijj_osmo-sync-shot found `001A`
    power_mode 0 ignored while asleep. Key 0x03 SnapShot must follow the wake-up advertisement.
    The DUML wake in osmosis and osmoControl is for an unconnected Osmo Nano and needs a separate
    DUML pairing.
  - **Tested (2026-10-14):**
    - `001A` power_mode 3 is acked with ret_code 0 (`AA 13 00 20 … 00 1A 00 …`). The screen goes
      dark and the link stays up.
    - From that sleep, 3 s of the wake-up advertisement wakes the camera: the start-up chime and
      animation play for 2–3 s. **The link stayed up** (no drop, unlike DJI's Q&A), and status pushes
      came back on it. A `1D03` start sent on the first push recorded as soon as live view appeared.
    - The wake-up drops Pre-Rec: the camera comes back unarmed. A QS press once it is back and
      idle arms it again, and a `1D03` start after that keeps the buffer.
    - A long press on the camera's power button while connected is the same sleep: `1D02` shows
      power_mode 3 and the wake-up advertisement wakes it. (An earlier "it doesn't wake" was a bridge
      bug that stopped the advertisement at once.)
    - After `001A`, one more push with power_mode 0 arrives (before the ack), then power_mode 3.
    - A wake-up 4 s after the sleep worked. No minimum delay seen so far.
    - Link: the camera starts at 15 ms and asks for 20 ms itself; it accepts the bridge's
      100–125 ms. The ELEMNT link runs at 40 ms. With the slow camera link, the ELEMNT's burst
      after connecting still congests its link briefly (a few dropped notifications, each
      congestion clears within about 60 ms), and record and sleep keep working.
- **Auto power-off** turns the camera off completely and ends BLE. A powered-off camera cannot be
  woken over BLE. AeroCam keeps the camera awake with a QS key press every 10–300 s (default 15 s).
  AeroCam treats the link as dead after 8 s without status (45 s in keep-awake mode).
- **Wake-up advertising** (from sleep only):
  - Advertise manufacturer data `0A FF 'W' 'K' 'P' <camera MAC, reversed>` for 2–3 s.
    Example for 60:60:1F:C7:50:96: `0A FF 57 4B 50 96 50 C7 1F 60 60` (`ble/ble.c`).
  - It must be connectable `ADV_IND`; DJI cameras filter on PDU type
    (DJI-Remote `docs/implementation.md`).
  - Conditions: the remote connected recently, and the camera has slept for 30 minutes or less.
  - Open issue #24 suggests the wake advertisement may need the same public address as the
    earlier connection. **Unverified.**
  - **Our constraint:** the classic ESP32 (Bluedroid) has one legacy advertising set. The wake
    advertisement would briefly replace the GoPro advertisement aimed at the ELEMNT.

## Code to build on

- **DJI's demo:** about 7.4k lines of C. The parts we need are small: `dji_protocol_parser.c`
  (416 lines), the two CRC files (80 + 80), the structs header (210) and parts of the descriptors.
  The rest is GPS, LED, buttons and a FreeRTOS data layer that `malloc`s every frame.
  - It targets ESP-IDF 5.5 and uses the Bluedroid GAP/GATTC APIs directly. These exist in IDF 4.4
    too, but the demo registers its own callbacks, which clash with the Arduino BLE library.
    Arduino 2.0.17 has `BLEDevice::setCustomGapHandler` / `setCustomGattcHandler` as hooks.
  - **License is ambiguous:** `LICENSE` offers the DJI EULA and MIT; source files say
    `SPDX-License-Identifier: MIT` but also carry a "proprietary … dissemination prohibited" notice.
  - Known issue: a 2048-byte task stack overflowed on ESP32-WROOM; 4096 fixed it (issue #3).
- **Cleaner references:** [rhoenschrat/DJI-Remote](https://github.com/rhoenschrat/DJI-Remote) (MIT),
  [KonradIT/osmosis](https://github.com/KonradIT/osmosis) (MIT; `RsdkProtocol.kt` is a compact,
  tested port of the framing, CRCs and payloads), node-osmo (MIT), djictl (CC0), AeroCam (GPLv3).
- **Risk (resolved):** the M5Stick author says Arduino BLE libraries "had quirks that made them
  incompatible with the DJI protocol" ([blog post](https://serialhobbyism.com/open-source-diy-remote-for-dji-osmo-action-5-pro-cameras)),
  without details. Two such quirks and their fixes are under Transport rules; after the fixes,
  Arduino BLE works.
- **Other repos checked:** node-osmo is DUML only (Mimo-style pairing, Wi-Fi, streaming, battery;
  no record control). djictl targets the Pocket 3 and its BLE battery command is marked as not
  working. xaionaro/reverse-engineering-dji has Pocket 3 captures only.

## Unverified

- Whether `1D03`'s device_id field matters (the demo's value works).
- Whether a `1D03` start in Photo mode takes a photo.
- Whether Pre-Rec survives a `1D04` mode switch or sleep.
- Minimum Action 5 Pro firmware.
- The exact `1D02` push rate.
- Whether the camera stores the pairing under mac, device_id or the BLE address.
- Whether the camera ever asks for SMP security, and whether Battery Service 0x180F is readable
  without bonding.
- Whether DUML battery pushes arrive with no pairing at all.
- Whether wake-up needs a public address, or the same address used for the earlier connection.
- Whether the Mimo app is locked out while the ESP32 is connected.

## Plan for the bridge

1. **Done (2026-10-14):** test sketch, camera side only (`src/dji_test.cpp`, `pio run -e dji-test`).
   Connect as a central with MTU 500, pair with Accept, subscribe to `1D02`, and log battery, mode
   and recording state. Start and stop recording from serial commands.
2. **Done (2026-10-14), tested with the ROAM 3 and the camera:** merged with the GoPro side
   (`src/main.cpp`, `pio run -e esp32dev`):
   - ELEMNT `SHUTTER on/off` → `1D03` start/stop.
   - `1D02` battery → GoPro status 70 and `0x2A19`.
   - `1D02` recording state and record time → GoPro status 10 and 13. Armed pre-rec (`0x05`) counts
     as not recording.
3. **In progress (2026-10-14):** sleep, wake-up and reconnects.
   - ELEMNT Sleep (GoPro command `05`, from the GoPro's page in My Sensors) → `001A` power_mode 3.
     **Tested.** While the camera sleeps, the same button wakes it (tested).
   - The bridge treats the camera as asleep when `1D02` shows power_mode 3 or no push has come for
     3 s. A sleeping camera keeps the link up and goes quiet. While it sleeps, nothing is sent to it,
     and the ELEMNT sees it as not recording.
   - ELEMNT record while the camera sleeps or is not connected → the wake-up advertisement (in place
     of the GoPro advertisement), then the start on the camera's first status push, if that comes
     within 30 s. A stop cancels a pending start. The advertisement runs until the camera is back,
     30 s at most. **Tested** from both kinds of sleep; the camera was back within 1–2 s.
   - The advertisement needs the camera's address, which the bridge keeps in NVS from the last
     connection. So a camera that sleeps through a bridge reboot can still be woken (**tested**: it
     woke). Pre-Rec is not armed again then: the bridge doesn't know whether it was armed.
   - **A sleeping camera keeps advertising and accepts a new connection, but answers nothing on it**
     (seen after a bridge reboot). Once, service discovery never finished, and the link stayed
     useless even after the camera woke up, until it dropped 49 s later. Another time, discovery,
     MTU 500 and the notification subscription worked, but the R SDK connection request got no
     answer. Awake, the camera answers it within 0.1 s, also before the user taps Accept.
     So the bridge gives up after 5 s without discovery or 8 s without an answer, waits 30 s before
     the next try, and while waking the camera tries again from 1.5 s in. **Tested:** the 5 s
     give-up (the disconnect ends the search with status 0x81, and the link drops with reason 0x16).
     The 8 s one is not tested yet.
     A third time, the sleeping camera did not show up in 44 s of scanning at all. ELEMNT record
     then woke it: found 0.4 s into the wake-up, paired at 3.5 s, recording at 5.5 s (**tested**).
   - If Pre-Rec was armed when the camera fell asleep, the bridge arms it again with a QS press once
     the camera is back and idle, before a pending start. **Tested**, with both wake-ups.
   - Reconnects: after any drop the bridge scans again and redoes the connection request
     (verify_mode 0) and the subscribe, as all the reference remotes do. **Tested:** camera off and
     on reconnects with no prompt, and the ELEMNT still controls it.
   - Not handled: the camera's Auto Power Off really powers it off, and it can't be woken from that
     (AeroCam). AeroCam's keep-awake presses QS, which here toggles Pre-Rec, so it is not an option.
