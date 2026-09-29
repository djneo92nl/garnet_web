# garnet_web

A two-column web settings UI for ESP32 in the Garnet (Palm OS 5) style
that also replaces WiFiManager. Groups are listed on the left and the
selected group's fields on the right. It renders every
[garnet_settings](../garnet_settings) group and adds built-in groups
behind build flags:

| Flag | Group |
|---|---|
| `GARNET_WEB_WIFI` | Wi-Fi: scan/join, 5 saved networks (strongest first), DHCP/static, captive-portal setup AP |
| `GARNET_WEB_ETH` | Ethernet (RMII, e.g. WT32-ETH01). Primary link: while it is up, WiFi STA is off |
| `GARNET_WEB_BT` | Bluetooth on/off + name. Compiled out on chips without BT (`SOC_BT_SUPPORTED`) |
| `GARNET_WEB_SD` | SD card info. The app mounts the card and calls `gwSetSd()` |
| (always) | System: hostname, chip/memory info, web password, backup/restore |
| `GARNET_WEB_UI_FS` | Serve the UI from a filesystem (`gwSetUiFs`) instead of flash |

It targets Arduino-ESP32 **3.x** only, through the pioarduino platform
(stock PlatformIO espressif32 is still 2.x).

## Layout

- `include/garnet_web.h`: public API (`GwConfig`, `gwBegin`, `gwLoop`, `gwSetEth`, `gwSetSd`).
- `src/gw_core.cpp`: begin/loop, the deferred-work queue, the shared lock and format helpers.
- `src/gw_server.cpp`: esp_http_server. Only 4 URI handlers are registered, and routing is string matching.
- `src/gw_auth.cpp`: salted SHA-256 password in NVS, RAM session tokens, lockout.
- `src/gw_net.cpp`: the uplink state machine (ETH → saved WiFi → setup AP), mDNS and captive DNS.
- `src/groups/`: the built-in `GsGroup` tables.
- `web/`: the frontend. Plain scripts, with Lit loaded as `window.Lit` from `web/vendor/lit.min.js`.
- `tools/build_ui.py`: inlines and gzips `web/` into `src/gw_ui_gz.h`. It is the committed PIO extraScript.
- `tools/mock_server.py`: a fake device for UI work (stdlib only).

## Rules

- **App callbacks run on the app's task.** The server task only enqueues
  them (`gwDeferChanged` / `gwDeferAction` / `gwDeferReboot`), and `gwLoop()`
  runs them. Apps therefore need no locking. The one exception is
  `GsField::info` getters, which run on the server task, so they must be
  read-only. Anything touching the WiFi driver also stays on the loop
  task. The server only sets flags such as `scanRequested`.
- **Network groups reboot on save.** Applying network settings live is where
  WiFiManager-style libraries get flaky, and a reboot is boring and reliable.
- **The web password is always required.** Without `defaultPassword` the server
  refuses to start. POSTs also need an `X-GW: 1` header, which acts as a CSRF guard.
- **The UI must work offline.** A captive portal has no internet, so there are
  no CDNs, and everything is vendored and inlined.
- **UI look:** Garnet / Palm OS 5, not iOS. Flat page, navy title tag
  (`--tag`) over a rule, bold `Label:` rows, 1px round-rect buttons,
  square checkboxes, `▼` popup triggers, dotted field underlines. Dark
  mode mirrors `garnetDarkVariant()` from garnet_ui_core. The copy is terse
  and device-like, with no chatty explanations. On wide screens the whole
  UI is a centered column.
- The WiFi scan uses the retry-on-`WIFI_SCAN_FAILED` workaround from
  garnet_ui's WiFi selector (`ESP_ERR_WIFI_STATE` race with `WiFi.begin`).
- Read the MAC from efuse (`esp_read_mac`), not `WiFi.macAddress()`. The
  latter returns zeros before the driver starts, which was hit on hardware
  as an AP named "-0000".
- Style follows the garnet repos: `gw*` / `Gw*`, `kConst`, anonymous
  namespaces, Arduino `String`, and comments that explain *why*.

## Verifying a change

- UI: `python3 tools/mock_server.py`, then open http://127.0.0.1:8080
  (the password is in `tools/fixtures/device.json`). The mock serves `web/`
  unbundled, so edit and reload. It re-implements the API, so keep it in
  sync with `gw_server.cpp`.
- Firmware: `pio run` in `examples/esp32s3`, `examples/wt32_eth01` and
  `examples/cyd`. The examples symlink this repo and `../garnet_settings`.
- After changing `web/`, commit the regenerated `src/gw_ui_gz.h`
  (`python3 tools/build_ui.py`). `--check` verifies it is current.
  PlatformIO's Python may link a different zlib than your shell's, so a
  PIO build can rewrite the header with different bytes for the same UI.
  That's harmless (only the ETag changes). Commit whichever one you have.

## Updating Lit

`web/vendor/lit.min.js` is Lit 3.3.3 bundled as an IIFE. Rebuild it in a
scratch directory with `npm i lit esbuild`, write an entry file containing
`export { LitElement, html, css, svg, nothing } from 'lit'`, then run
`npx esbuild entry.js --bundle --minify --format=iife --global-name=Lit`.

## Not yet

- `library.json` doesn't declare its `garnet_settings` dependency, because
  that repo isn't on GitHub yet. Add it once it's pushed.
- The garnet_ui / garnet_ui_lvgl menu adapter for garnet_settings.
