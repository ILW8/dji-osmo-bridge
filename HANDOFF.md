# Handoff: DJI Osmo battery on Wahoo ELEMNT ROAM 3

Status as of 2026-10-13. **The ELEMNT side works.** The bridge's battery level shows on the ROAM 3 as a
percentage on My Sensors and as the green ring around the sensor icon. The ring updates as soon as the
value changes. Next is the camera side.

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
- Serial monitor at 115200. Type `0`–`100` + Enter to set the reported battery.
- Flash use is about 86% (the Bluedroid library is large). If space runs out, add `board_build.partitions = huge_app.csv`.
- CLion's clang errors such as `'Arduino.h' file not found` are an indexing problem only. The PlatformIO build is fine.
- Git: `user.email` is set locally to `ilw8@41f.dev`.

## What `src/main.cpp` does now

- Advertises as **`TRACKR RADAR`** with the Cycling Speed and Cadence service (0x1816). The name is what
  makes the battery work (see below). The CSC service is still advertised but **never sends a measurement**.
- Battery Service (0x180F / 0x2A19). The level starts at 42 and can be set over serial.
  On each change it sends one notification. It forces this with `setNotifications(true)` on its own CCCD,
  because the ELEMNT never subscribes.
- Device Information Service (manufacturer `DIY`, model `Osmo Battery Bridge`, firmware `0.1`).
- Logs every GATT read and write by handle.

Handle map printed at boot: cadence measurement **42** (CCCD **43**), cadence feature **45**, battery level **57** (CCCD **58**).
The ELEMNT also reads **72, 74, 76** (Device Information).

Observed ELEMNT behaviour with the current firmware: writes `01 00` to 43 and reads 72/74/76 and 57 at connect.
After that it re-reads 57 at irregular intervals. It never writes to 58. Battery notifications are accepted anyway.

## How the ELEMNT firmware handles BLE sensor battery

Source: Wahoo's BoltApp APK, decompiled with jadx 1.5.6 (outside the repo, never installed or run).
Version 1.77.10.36, build 18001, from <https://bolt.wahoofitness.com/boltapp/version.json>.
Its device-type enum lists BOLT, ROAM, BOLT_2, ROAM_1_1, ROAM_2 and ACE, but **no ROAM 3**. The ROAM 3
behaved exactly as this code predicts, so the sensor stack is probably shared.
About 229 methods failed to decompile. None were on the paths below.

1. **Classifying the device in discovery.** The ELEMNT checks the advertised service UUIDs first. It knows
   0x180D HR, 0x1816 CSC, 0x1818 power, 0x1814 footpod, 0x1826 FTMS, 0x1809 core temp,
   0xFEA6 GoPro, and Wahoo 0xEE06 / 0xEE07 / 0xEE0A.
   It reads only characters 4–8 of a UUID, so a 128-bit UUID just acts as one of these. If no service
   matches, it matches the **device name** against a per-product regex table instead.
2. **Product type.** At connect the BLE product type is the **name-matched product if the name matches**.
   Otherwise it is the generic product for the service category. The name wins even when the device
   advertises CSC. Device Information, advert manufacturer data and GAP appearance do not change the type.
3. **Battery gate.** The battery helper is attached to every BLE sensor. It reads 0x2A19 (and 0x2A1A / 0x2A1B)
   after discovery. Values are **silently dropped unless the product type's category is**
   bike computer, gear shifting, light, e-bike, radar, power or GoPro. HR passes only for TICKR FIT and
   TRACKR HR / HR 2. Speed and cadence sensors fail the gate. That is why all early experiments (CSC,
   battery 42 / 5, forced notifications, DIS) left the ring empty of real data.
4. **Display.** My Sensors shows `<n>%` when a value exists. The ring follows the same value.
   Levels: most sensors GOOD ≥51, LOW 11–50, CRITICAL 1–10. Power pedals and Di2 use their own cutoffs.
5. **ANT+ switch.** For Wahoo products, a 4–5 hex-digit name suffix is parsed as an **ANT+ device ID**.
   The ELEMNT then replaces the BLE link with ANT+ links. For TRACKR RADAR those are ANT+ radar and light.
   `TRACKR RADAR 0A5B` paired as "ANT+ ID 2651" and sat on "Searching...". With **no suffix**, no ID is
   parsed and the link stays BLE.

### Names and services that pass the gate

| Route | Usable? |
|---|---|
| Name `TRACKR RADAR` or `MIRROR` (no suffix) → Wahoo TRACKR radar | **Yes. This is what we use.** |
| Power names (`Stages`, `PowerCal`, `ASSIOMA…`, `POWRLINK ZERO xxxx`, …), or service 0x1818 with any name | No. The ELEMNT treats it as a power meter. |
| HR names (`TICKR FIT xxxx`, `TRACKR HR`, `TRACKR HR 2`) | No. It would compete with the real HR strap. |
| `ELEMNT …` names, or service 0xEE06 | No. The ELEMNT treats it as another Wahoo head unit (0xEE06 is also behind a feature flag). |
| `GoPro` / `GoPro xxxx`, or service 0xFEA6 with any name | Passes and stays BLE, but the ELEMNT would try to control it as a camera. Untested. |

There is no field that gives a battery gauge with an arbitrary name without claiming to be one of these.
Generic light, radar, e-bike and gear-shifting types can only be assigned over ANT+.

## Open questions

- What the ELEMNT does long-term with a "radar" that has no radar service. So far no radar UI or alerts
  were reported, but this has not been checked on a ride, and neither has whether radar fields appear in the FIT file.
- Whether the connection stays up while idle, now that the bridge sends nothing between battery changes.
  If it drops, put back a once-a-second empty CSC measurement (flags `0x00`).

## Camera side (not started)

- DJI's official BLE remote demo: [dji-sdk/Osmo-GPS-Controller-Demo](https://github.com/dji-sdk/Osmo-GPS-Controller-Demo). It is an ESP-IDF project with sdkconfig defaults for the C2, C3 and S3. Building it for a classic ESP32 is untested.
  Switching PlatformIO to ESP-IDF means changing `framework = arduino` to `framework = espidf`, and the code changes with it.
- Other references: [datagutt/node-osmo](https://github.com/datagutt/node-osmo) (TypeScript), [xaionaro-go/djictl](https://github.com/xaionaro-go/djictl) (Go CLI), and [xaionaro/reverse-engineering-dji](https://github.com/xaionaro/reverse-engineering-dji).
- **Not yet verified** that the DJI BLE protocol exposes the camera's battery percentage. Check this first.
- The ESP32 must be a BLE central (to the camera) and a peripheral (to the ELEMNT) at the same time.
- A Karoo extension already exists: [derFrisson/Karoo-DJI-Osmo-extension](https://github.com/derFrisson/Karoo-DJI-Osmo-extension) (early stage). It is only relevant if switching head units.

## Gotchas

- Python heredocs run from Git Bash turned `\\n` escapes into real newlines in C++ strings. Use the Edit tool for source changes.
- CLion keeps rewriting `.idea/*.iml` and adding `.idea/*.xml` files. These are unrelated to the investigation.
- After changing the bridge's name or type, **forget the sensor on the ELEMNT and pair again**.
- Do not copy Wahoo code into this repo. Summarize findings only.
