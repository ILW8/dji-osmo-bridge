# dji-osmo-bridge

ESP32 firmware that lets a **Wahoo ELEMNT ROAM 3** bike computer show the battery of a **DJI Osmo
Action 5 Pro** and control it.

The ELEMNT has no way to add third-party sensors or data fields, but it supports GoPro cameras. The
bridge connects to the Osmo over BLE with DJI's R SDK protocol, and presents itself to the ELEMNT as a
GoPro HERO12 Black named "GoPro 0042". The ELEMNT then shows:

- the Osmo's battery, on My Sensors and on the GoPro page;
- the Osmo's recording state and record time.

The ELEMNT's record and stop buttons start and stop recording on the Osmo, also with Pre-Rec armed. Its
Sleep button puts the camera to sleep and wakes it again. The bridge reconnects to the camera on its own.

## Build and flash

Needs a classic ESP32 (PlatformIO board `esp32dev`) and PlatformIO.

```sh
pio run -t upload -t monitor -e esp32dev   # the bridge (src/main.cpp)
pio run -t upload -t monitor -e dji-test   # camera-side test sketch (src/dji_test.cpp)
```

On first use, tap Accept on the camera when the bridge asks to pair, and add the bridge on the ELEMNT
as a GoPro.

## Docs

- [HANDOFF.md](HANDOFF.md): status, how the ELEMNT treats sensors, and the BLE gotchas found on the way.
- [DJI_PROTOCOL.md](DJI_PROTOCOL.md): the Osmo's BLE protocol as used here.

Not affiliated with DJI, Wahoo or GoPro.
