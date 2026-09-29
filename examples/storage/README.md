# storage

The firmware to flash on a spare ESP32 before it goes in the drawer.
When you pick the board up again, it's also a bench tool for finding out
what the board is and what it's wired to. It contains:

- **Wi-Fi + web UI + OTA**: put it on your network, install the real firmware.
- **Files**: the flash data partition (LittleFS, 1.4 MB) for datasheets,
  pinout photos and configs.
- **Hardware**: I2C scan on any two pins, with likely part names. A GPIO
  tester (read, pull-up/down, drive high/low, ADC in mV) and a read of every
  free pin. Flash/PSRAM pins are refused; console pins are read-only.
- **Serial**: a monitor for a second UART on pins of your choice. Text or
  hex view, and a send line with a choice of line ending. 74880 baud is in
  the list for reading another ESP's boot ROM.
- **Log**: the board's own log, live in the browser.
- **Clock**: NTP with a time-zone picker, or "Use Browser Time" when it's on
  its own setup AP.

One source builds for every Wi-Fi capable chip. It touches no pins until
you ask it to, so it's harmless on any board.

| env | chip | image |
|---|---|---|
| `esp32` | ESP32 | 1.21 MB |
| `esp32s2` | ESP32-S2 | 1.15 MB |
| `esp32s3` | ESP32-S3 | 1.21 MB |
| `esp32c3` | ESP32-C3 | 1.29 MB |
| `esp32c5` | ESP32-C5 | 1.26 MB, without Clock and Hardware |
| `esp32c6` | ESP32-C6 | 1.29 MB |

Each OTA slot is 1.31 MB. The C3 and C6 have 17–24 KB left. The C5's
Wi-Fi 6 stack and drivers are bigger, so its build leaves out Clock and
Hardware to fit.

The ESP32-H2 has no Wi-Fi and the ESP32-P4 has no radio of its own, so
neither is included. A bare "start an AP" Arduino sketch is already
0.9–1.0 MB. garnet_web, LittleFS and OTA add the rest.

```
pio run -e esp32c3 -t upload
```

## Taking a board out of storage

1. Power it. Over serial it prints one line, e.g.
   `[storage] ESP32-S3 setup AP  id 2884855F1E08  http://192.168.4.1/`
2. Join the open AP `<chip>-XXXX` (e.g. `ESP32-C3-1E08`). The captive
   portal opens.
3. Sign in with `changeme`.
   - The **Board** group has the label and notes you left.
   - **System** has the chip, flash, PSRAM and Chip ID.
   - **Files** holds whatever you stored on the flash (1.4 MB), e.g. a
     datasheet, a pinout photo or the project's config.
4. Optionally put it on Wi-Fi.
5. Go to System → Firmware → **Update…** and choose the new project's
   `firmware.bin`.

## Partition table: read this before shelving

The table is `default.csv`, with two 1.25 MB OTA slots in the first 4 MB.
It works on every module.

**OTA cannot change the partition table.** A firmware installed through
Update… keeps this table, so the future project must:

- be at most **1.25 MB** (System shows `Firmware slot … max 1.25 MB`), and
- be built with the same table (`board_build.partitions = default.csv`),
  or with one that has identical offsets.

If you know a board will get a bigger project, shelve it with a bigger
table instead, for example on 8 MB or 16 MB modules:

```ini
board_build.partitions = default_8MB.csv   ; 2 x 3.2 MB slots
board_upload.flash_size = 8MB
```

Anything else (a camera build with a 16 MB table, for instance) has to be
flashed over USB once. That's still fine: the storage firmware has done its
job of telling you what the board is.

Saved Wi-Fi networks, the label and the notes live in NVS, which is at the
same offset in all the default tables. A new firmware using garnet_web
will see the saved networks and join them right away.
