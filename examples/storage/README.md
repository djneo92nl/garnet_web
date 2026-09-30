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

| env | chip | image | free in slot | tools left out |
|---|---|---|---|---|
| `esp32` | ESP32 | 1.20 MB | 103 KB | none |
| `esp32s2` | ESP32-S2 | 1.14 MB | 162 KB | none |
| `esp32s3` | ESP32-S3 | 1.20 MB | 110 KB | none |
| `esp32c3` | ESP32-C3 | 1.28 MB | 27 KB | Clock |
| `esp32c5` | ESP32-C5 | 1.31 MB | 4 KB | Clock, Hardware |
| `esp32c6` | ESP32-C6 | 1.30 MB | 7 KB | Clock |
| `esp32p4` | ESP32-P4 (Ethernet only) | 1.22 MB | 90 KB | Wi-Fi |

Each OTA slot in `default.csv` is 1.31 MB. The RISC-V chips' Wi-Fi stacks
are bigger, so those builds leave out the heaviest tools to fit
(`[tools]` in `platformio.ini`). The C5 and C6 are close to the limit:
anything added to garnet_web lands on them first.

The ESP32-H2 has no Wi-Fi, so it isn't included.

### ESP32-P4

`esp32p4` targets boards on Espressif's reference layout (tested:
Waveshare ESP32-P4-Module-DEV-KIT, chip v1.3). Ethernet runs on the
variant's RMII pins, and the console is on the P4's USB port. The prebuilt
core supports P4 chips below revision v3.0 only.

It is **Ethernet only for now.** The P4's Wi-Fi is an on-board ESP32-C6
running ESP-Hosted over SDIO. On the tested board:

- The C6 shipped with old ESP-Hosted firmware; the host warned "Version on
  Host is NEWER".
- After updating it to v2.6.4 over SDIO, the C6 crashes on its first Wi-Fi
  call (`WifiGetProtocol`). The P4 then resets ("Unrecoverable host sdio
  state") every time Wi-Fi starts.
- The v2.12.13 image that matches Arduino 3.3.12 is rejected by the C6
  (`ESP_ERR_OTA_VALIDATE_FAILED`). Most likely it's larger than the C6's
  factory OTA slot, which an OTA update cannot enlarge.

The fix is to reflash the C6 directly over the board's **ESP32-C6 UART
header**, with esptool and a complete image (bootloader, partition table,
app). After that, add `GARNET_WEB_WIFI` back to `env:esp32p4`. The web UI's
System → Firmware → *Update Wi-Fi Co-processor…* stays available (it only
starts the SDIO link, never Wi-Fi), but it can't get past the slot-size
limit.

```
pio run -e esp32c3 -t upload
```

## Taking a board out of storage

1. Power it. Over serial it prints one line, e.g.
   `[storage] ESP32-S3 setup AP  id 2884855F1E08  http://192.168.4.1/`
2. Join the open AP `<chip family>-XXXX` (e.g. `ESP32-A1F0`, `ESP32-C3-1E08`). The captive
   portal opens.
3. Sign in with `changeme`.
   - The **Board** group has the label and notes you left.
   - **System** has the chip, flash, PSRAM and Chip ID.
   - **Files** holds whatever you stored on the flash (1.4 MB), e.g. a
     datasheet, a pinout photo or the project's config.
4. Optionally put it on Wi-Fi, from the web UI or over USB without the
   setup AP (any serial monitor at 115200, or a script):

   ```
   gw wifi "My Network" "password"   save, restart, join
   gw status                         network, IP, hostname, chip ID
   gw forget "My Network"
   gw password reset                 forgot the web password? back to the default
   ```

   Replies start with `OK` or `ERR`. For a drawer of boards:
   `for p in /dev/cu.usbserial-*; do printf 'gw wifi "My Network" "password"\n' > $p; done`
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
