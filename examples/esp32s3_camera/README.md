# esp32s3_camera

garnet_web on an ESP32-S3 camera board: a live camera view with its
settings, SD card info, Wi-Fi with setup AP, and BLE. It shows how an
app adds its own group, and how it serves something heavy (an MJPEG
stream) on a separate server without blocking the settings UI.

Tested on an ESP32-S3-WROOM-1 N16R8 "CAM" board (CH343 USB, 16 MB flash,
8 MB octal PSRAM) with an OV3660 module. The OV5640 and OV2640 use the
same code path.

## What it shows

| Group | Contents |
|---|---|
| Camera | Live view, sensor + fps, resolution (QVGA to QXGA), JPEG quality, brightness/contrast/saturation, mirror, flip, snapshot to SD |
| Demo | One field of every garnet_settings type |
| Wi-Fi, Bluetooth, SD Card, System | Built-in garnet_web groups |

- Camera settings apply live on Save, through the group's `onSave` hook.
  They need no reboot.
- The stream is at `http://<device>:81/stream` and a single JPEG at
  `:81/capture`. Both are behind the web login, checked with `gwAuthorized()`.
- The UI shows the stream through the group's `img::81/stream` widget. It
  stops the stream while the browser tab is hidden.

## Wiring (on-board)

| Camera | GPIO | | SD (SDMMC, 1-bit) | GPIO |
|---|---|---|---|---|
| XCLK | 15 | | CLK | 39 |
| SIOD / SIOC | 4 / 5 | | CMD | 38 |
| D7..D0 | 16 17 18 12 10 8 9 11 | | D0 | 40 |
| VSYNC / HREF / PCLK | 6 / 7 / 13 | | | |

The camera pins are verified on hardware. The SD pins are not yet: the
card was untested when this was written. Change them in
`src/camera_group.cpp` for other boards.

## Run

```
pio run -t upload -t monitor
```

On first boot there is no Wi-Fi yet:

1. Join the open AP **S3 Demo-XXXX**. The captive portal opens.
2. Sign in with `changeme`.
3. Pick your network under Wi-Fi. The device restarts and joins it.

After that, use `http://s3-demo-xxxx.local`.
