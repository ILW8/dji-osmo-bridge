# Handoff: DJI Osmo battery on Wahoo ELEMNT ROAM 3

Status as of 2026-10-14. **The ELEMNT side works, now as an emulated GoPro.** The ROAM 3 pairs with the
bridge as a GoPro. The battery shows on My Sensors and as the ring around the sensor icon, and the
GoPro page shows battery, mode and preset. The record and stop buttons reach the bridge. Next is the
camera side: the bridge will pass those commands on to the Osmo and report the Osmo's battery and
recording state back.
The earlier radar route (name `TRACKR RADAR`, battery only) works too and is kept in commit `7ff56a4`.

## Goal and constraints

- Show the battery level of a **DJI Osmo Action 5 Pro** on a **Wahoo ELEMNT ROAM 3**.
- An ESP32 bridge connects to the camera over BLE and presents itself to the ELEMNT as a sensor.
  The ELEMNT shows the sensor's battery gauge, and that gauge should be the camera's battery.
- **No fake core metrics** (cadence, speed, HR, power) may be recorded. A real ANT+ HR strap is used.
  Extra radar data in activities is acceptable.
- Wahoo ELEMNT firmware is closed and has no SDK for third-party data fields.

## Hardware and tooling

- Elegoo "ESP-32" board, a classic ESP32 (WROOM-32 class, BLE 4.2). PlatformIO board `esp32dev`.
- PlatformIO in CLion, `framework = arduino`, Arduino-ESP32 core 2.0.17 (Bluedroid BLE library).
- `pio` is not on PATH. Use `~/.platformio/penv/Scripts/pio.exe run` (add `-t upload`, or use `device monitor`).
  Upload fails with `could not open port 'COM3'` while a serial monitor holds the port.
  **The user flashes the board.** Assistants build only (`pio run`) and hand off.
- Ghidra 12.1.4 is in `~/tools/ghidra_12.1.4_PUBLIC`. It needs JDK 21: set
  `JAVA_HOME="C:/Program Files/Java/jdk-21"` for the command (the system `JAVA_HOME` is JDK 17).
  REA's Ghidra provider only accepts PE files on Windows, so run `support/analyzeHeadless.bat` directly.
  `cmd.exe` mangles `|` in arguments, so pass several regexes as separate arguments instead of one with `|`.
- Serial monitor at 115200. Type `0`–`100` + Enter to set the reported battery.
- Flash use is about 86% (the Bluedroid library is large). If space runs out, add `board_build.partitions = huge_app.csv`.
- CLion's clang errors such as `'Arduino.h' file not found` are an indexing problem only. The PlatformIO build is fine.
- Git: `user.email` is set locally to `ilw8@41f.dev`.

## What `src/main.cpp` does now (GoPro emulation)

- **Bluetooth address** `02:60:50:00:00:42`, set with `esp_base_mac_addr_set` (the Bluetooth address is
  the base + 2). The ELEMNT keeps a capability store per address that survives "forget sensor". The
  factory address still carried the cadence capabilities, so a wheel-circumference setting showed up on
  the GoPro. To start clean again, change the last byte of `base`.
- **Advertising** copies a real GoPro (taken from GoPro's own test data in the Open GoPro repo):
  - Advertisement: service `0xFEA6` and manufacturer data for company `0x02F2`:
    schema `02`, status `05` (powered on + pairing mode), camera ID `62` (HERO12 Black), capabilities `33`,
    `00`, a 6-byte ID hash, media offload `0f`.
  - Scan response: name `GoPro 0042`, and `0xFEA6` service data = 4 bytes of the Wi-Fi MAC + `"0042"`.
  - Without the manufacturer data and pairing bit, the ROAM 3 did not list the device at all.
- **Bonding:** accepts Just Works bonding. The ELEMNT bonds right after connecting to a GoPro.
- **GATT:** the Open GoPro service with `b5f90072` (command), `73` (command response), `74` (setting),
  `75` (setting response), `76` (query) and `77` (query response). Also Battery Service (0x180F/0x2A19)
  for the sensor ring, and Device Information.
- **Replies** to the ELEMNT:
  - Command `3C`, Get Hardware Info: a HERO12 Black with firmware `H23.01.02.32.00`,
    serial `C3501324500042`, SSID `GP24500042`. The format is length-prefixed fields, as in the Open GoPro
    Python SDK.
  - Settings: resolution 4K (2=1), 30 fps (3=8). Everything else is 0.
  - Statuses: 1 (battery present), 2 (battery bars), 10 (recording), 13 (recording duration),
    33 (SD card OK), 34 (999 photos), 35 (7200 s left), 70 (battery %),
    96 (preset group 1000 = video), 97 (active preset 1). The length per status follows the Open GoPro spec.
  - Protobuf `f5 72` (GetPresetStatus): one video group holding one "Standard" preset.
    Other protobuf commands get ResponseGeneric SUCCESS.
  - Command `01` (shutter) logs `SHUTTER on/off`. The bridge then sends status 10 and 13 to the ELEMNT
    (as `0x93`), but only if the ELEMNT registered for them with query `0x53`.
- **Framing:** messages are split into 20-byte notifications, because the ELEMNT never negotiates a
  larger MTU. A message that needs more than one packet **must** use the 13-bit header (`0x20 | len>>8`).
  The ELEMNT reassembles packets only when the first byte has `0x20` or `0x80` set. A split message with the
  5-bit header was parsed as its first packet alone, which is why the 21-byte preset list was lost at first.
- **Serial:** `0`–`100` + Enter sets the battery. The bridge notifies `0x2A19` and sends status 70.
- Logs every GATT read and write by handle, every reply, and the bonding result.

### Observed ELEMNT ↔ GoPro session

1. After bonding it subscribes to the three response characteristics.
2. It sends: hardware info `3C`, then get settings 59 and 162, then registers for settings 2, 3, 121 and 122,
   then registers for statuses 10, 96 and 97, then 34, 35, 70 and 2, then 70, 13, 38, 39 and 33, then
   gets presets with `f5 72`.
3. **The whole sequence runs twice**, even with correct replies. In the legacy build's native code, reaching
   connection state 2 clears all cached values and registers again (`FUN_0035f360`).
   That is probably the second run.
4. Then it polls statuses 34, 35 and 70 and statuses 38 and 39 every few seconds, and gets the presets
   about every 15 s.
5. It writes setting `0x5B` with value `0x42` from time to time. The meaning is unknown. The bridge answers OK.
6. Record and stop send command `01 01 01` and `01 01 00`. After a status 10 change it polls 34, 35, 70, 38 and 39.
7. Pairing takes about 10–15 s. The first connect after reflashing can fail bonding with reason `0x66`,
   probably stale keys on the ESP32. The next connect bonds.

### GoPro page display quirks (ELEMNT side, seen on the ROAM 3)

- **Delayed battery readout:** the GoPro page battery does not follow changes. It stayed at 42% after the
  bridge reported 30%, even after switching pages or starting a recording. The value does reach the ELEMNT:
  the sensor ring updates at once, and a full reload shows it.
  **Workaround:** side button → My Sensors → select the GoPro (the gauge animates) → dismiss → back to
  the GoPro page. It now shows the new value. Opening My Sensors without selecting the GoPro is not enough.
- **Recording indicator lags:** after pressing record or stop, the red indicator does not change until
  you switch pages and come back.
- The Preset field shows "Standard" with grayed-out arrows, because only one preset is reported.
- Our status updates are decoded: a status 10 update makes the ELEMNT poll statuses at once. In the
  legacy build's native code, a value change sets a redraw flag and fires a "config changed" event
  (`notify_event(29, 0)`). The GoPro page listens for that event. Why the ROAM 3 does not redraw is
  unknown. The GoPro details here come from **build 18001**, the legacy build, and the native GoPro code
  of 302826 was not read. A real GoPro probably shows the same lag.

## How the ELEMNT firmware handles BLE sensor battery

Source: the ROAM 3's own app, `com.wahoofitness.bolt` 3.26.5.26 (build 302826, SHA-256 `cc2ee9a6…11c6ef`).
The ROAM 3 shows `WH61-302826` in Device Info (ROM layer `WH61`, app build `302826`). The APK is on the unit's
MTP storage at `system_update_elemnt/302826/BoltApp` (no file extension). ADB is off (USB `18D1:4EE1`,
MTP only), so the file is not hash-checked against the installed copy.
It was decompiled with jadx (from the jadx-headless-mcp 0.7.1 JAR, 679 errors) and `libCruxAndroid.so`
(armeabi-v7a, exported symbols) was disassembled with capstone. All of this ran outside the repo, and
nothing was installed or run on the device.
An earlier pass used build 18001 (1.77.10.36) from <https://bolt.wahoofitness.com/boltapp/version.json>.
That build is the BOLT 1/ROAM 1 line and is **not** what the ROAM 3 runs. Differences are noted below.

1. **Classifying the device in discovery.** The ELEMNT checks the advertised services first, in this
   order: Wahoo BOLT (ELEMNT, behind a feature flag), 0x1818 power, 0x1816 CSC, 0x1826 FTMS,
   FlowBio (`D24FF560-0F8C-4B3A-9E6F-09A5D12E68C4` and a session service `A34BF550-…`) as sweat,
   0xFEA6 GoPro, hDrop as sweat, 0x180D HR, MCUmgr as DFU, 0x1814 footpod, 0x1809 thermometer,
   VitalPro as respiratory, Wahoo DFU, and Wahoo Gym Connect as FTMS. If no service matches, it matches
   the **device name** against a per-product regex table instead.
   Changed from 18001: this is now Kotlin, and the UUID lookup is native (`from_full_uuid`). The
   18001 "only characters 4–8" shortcut has not been re-checked. FlowBio, hDrop and VitalPro are new.
   A native BLE Magene radar client (service `0xCC01`) exists, but `0xCC01` is **not** in this list.
2. **Product type.** At connect the BLE product type is the **name-matched product if the name matches**.
   Otherwise it is the generic product for the service category. The name wins even when the device
   advertises CSC. Unchanged in 302826: `TRACKR RADAR` is product 122 with the same regex.
3. **Battery gate.** The battery helper (`Batt_Helper`) is attached to every BLE sensor. It reads 0x2A19
   (and 0x2A1A / 0x2A1B) after discovery. Values are **silently dropped unless** native
   `crux_sensor_product_type_show_battery(product)` returns true.
   In 18001 this check was in Java; in 302826 it is native. Decoded:
   - **Blocked by product ID:** KICKR, KICKR SNAP, CORE, BIKE, BIKE SHIFT, ROLLR, MOVE, and hDrop.
   - **Otherwise passes when the sensor type is** power, ELEMNT, gear selection, radar, LEV (e-bike),
     light, GoPro, sweat or respiratory.
   - HR passes only for TICKR FIT (63), TRACKR HR (114) and TRACKR HR 2 (133).
   - Everything else fails, including speed/cadence (`GENERIC_BIKE_SPEED_CADENCE` has sensor type 4).
   `TRACKR RADAR` (122) has sensor type 20 (radar), so it passes. That matches the device's behaviour.
   This is why all early experiments (CSC, battery 42 / 5, forced notifications, DIS) left the ring empty.
4. **Display.** My Sensors shows `<n>%` when a value exists. The ring follows the same value.
   Levels: most sensors GOOD ≥51, LOW 11–50, CRITICAL 1–10. Power pedals and Di2 use their own cutoffs.
5. **ANT+ switch.** For Wahoo products, a 4–5 hex-digit name suffix is parsed as an **ANT+ device ID**.
   The ELEMNT then replaces the BLE link with ANT+ links. For TRACKR RADAR those are ANT+ radar and light.
   `TRACKR RADAR 0A5B` paired as "ANT+ ID 2651" and sat on "Searching...". With **no suffix**, no ID is
   parsed and the link stays BLE.
   302826: the level cutoffs (4) and the TRACKR RADAR → ANT+ radar and light switch (5) are unchanged.
   The suffix parser itself was not re-read; the observed pairing behaviour matches it.

### Names and services that pass the gate

| Route | Usable? |
|---|---|
| Name `TRACKR RADAR` or `MIRROR` (no suffix) → Wahoo TRACKR radar | **Yes. This is what we use.** |
| Power names (`Stages`, `PowerCal`, `ASSIOMA…`, `POWRLINK ZERO xxxx`, …), or service 0x1818 with any name | No. The ELEMNT treats it as a power meter. |
| HR names (`TICKR FIT xxxx`, `TRACKR HR`, `TRACKR HR 2`) | No. It would compete with the real HR strap. |
| `ELEMNT …` names, or service 0xEE06 | No. The ELEMNT treats it as another Wahoo head unit (0xEE06 is also behind a feature flag). |
| Service 0xFEA6 + GoPro manufacturer data, in pairing mode (name `GoPro 0042`) → `GOPRO` | **Yes. This is what we use now** (see "What `src/main.cpp` does now"). The ELEMNT controls it as a camera, which is what the Osmo bridge needs. Whether an arbitrary name works was not tested. |
| FlowBio service `D24FF560-…` with any name, and no CSC/power/FTMS service (those win) → `FLOWBIO` (135, sweat) | Passes the gate in 302826 and would allow any name. The ELEMNT would expect a FlowBio sweat sensor and may show or record sweat fields. Untested. |
| VitalPro service → `VITALPRO_BREATHING` (137, respiratory) | Passes the gate. Its UUID was not found as a string. Untested. |
| hDrop (sweat) | No. Blocked by product ID. |

Apart from the FlowBio/VitalPro routes above, no route gives a battery gauge with an arbitrary name
without claiming to be one of these. Generic light, radar, e-bike and gear-shifting types can only be
assigned over ANT+: no BLE service maps to them, and `0xCC01` Magene is not in the discovery list.

## Open questions

- Why the GoPro page does not redraw on status changes (see "GoPro page display quirks").
  Reading the native GoPro code of build 302826 may answer it.
- Whether the GoPro activity records anything in the FIT file (camera events). Not checked on a ride.
- Whether the GoPro route works with an arbitrary name, for example "Osmo Action 5 Pro".
  The legacy native library has a `GoPro (#*)` string.
- Radar route only: what the ELEMNT does long-term with a "radar" that has no radar service.

## Camera side (researched, not built)

The full protocol is in **[DJI_PROTOCOL.md](DJI_PROTOCOL.md)**: frame format and CRCs, transport rules,
pairing, the `1D02` status push, `1D03` record, sleep and wake-up, references, unverified points, and the
bridge plan.

In short: the Action 5 Pro speaks DJI's open "R SDK" BLE protocol. Battery % is byte 37 of the `1D02`
status push, recording state is byte 1, and `1D03` starts and stops recording. Pairing happens inside
the protocol: the user taps Accept on the camera once. Each frame must fit in one BLE packet, so the
bridge needs a large MTU to the camera. The ESP32 must be a BLE central (to the camera) and a
peripheral (to the ELEMNT) at the same time.

Next step: an Osmo-only test sketch as a second PlatformIO environment (step 1 of the plan in
DJI_PROTOCOL.md). Merge it with the GoPro bridge after that.
- A Karoo extension already exists: [derFrisson/Karoo-DJI-Osmo-extension](https://github.com/derFrisson/Karoo-DJI-Osmo-extension) (early stage). It is only relevant if switching head units.

## Gotchas

- Python heredocs run from Git Bash turned `\\n` escapes into real newlines in C++ strings. Use the Edit tool for source changes.
- CLion keeps rewriting `.idea/*.iml` and adding `.idea/*.xml` files. These are unrelated to the investigation.
- After changing the bridge's name or type, **forget the sensor on the ELEMNT and pair again**.
  Forgetting does not clear the ELEMNT's per-address capability store. Change the Bluetooth address for a clean slate.
- The serial monitor sometimes shows a run of `�` characters. The connection carries on, so it is line noise
  and not a reboot.
- Do not copy Wahoo code into this repo. Summarize findings only.
