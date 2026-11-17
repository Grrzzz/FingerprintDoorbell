# AGENTS.md

Guide for AI coding agents (and humans) working on this repository. It is the fork **Grrzzz/FingerprintDoorbell** of frickelzeugs/FingerprintDoorbell: an ESP32 + Grow R503 fingerprint reader that doubles as a doorbell, with a web interface, MQTT and Home Assistant auto discovery. `README.md` describes the features for users, this file explains how to change the code without breaking it.

## Stack at a glance

- ESP32 (`esp32doit-devkit-v1`), Arduino framework, PlatformIO (`espressif32@6.13.0`, arduino-esp32 2.x). C++ of the Arduino core, no exceptions.
- Web server: ESP Async WebServer. The web interface is a plain HTML/CSS/JS single page app (no framework, no build step, no external requests) served from SPIFFS.
- MQTT with PubSubClient, fingerprint sensor with the Adafruit Fingerprint library, settings in NVS (`Preferences`).
- No unit tests. Verify firmware changes by building, UI changes with the virtual device (see [Testing](#testing)).

## Repository layout

| Path | What lives there |
| --- | --- |
| `src/main.cpp` | `setup`/`loop`, Wi-Fi, all HTTP routes, JSON status/settings, MQTT callbacks, scan/enroll flow, LED helpers |
| `src/FingerprintManager.*` | sensor access, fingerprint list, per-fingerprint policy (switch + schedule), storage of names and policies, LED styles |
| `src/SettingsManager.*` | settings structs (`AppSettings`, `WifiSettings`, `RingSettings`) and their NVS keys |
| `src/HomeAssistantDiscovery.*` | table of Home Assistant entities and the discovery/state publishing |
| `src/SelfUpdate.*`, `src/GithubRoots.h` | one-click update (HTTPS download of `update.zip` from GitHub releases), manual upload with the ZIP reader, and the root certificates |
| `data/` | web interface: `app.html`, `app.js`, `style.css`, plus `wificonfig.html` (Wi-Fi setup mode, still templated by the server) |
| `lib/` | private libraries (none at the moment), only a README is tracked |
| `partitions.csv` | the partition table every released version uses, see below |
| `tools/` | virtual device, screenshot script and `make_update_zip.py` (builds the release `update.zip`) |
| `doc/` | images for the README, `bootloader.zip` for esptool flashing |

## Build

```
pio run                  # builds firmware.bin
pio run -t buildfs       # builds spiffs.bin from data/ (web interface)
```

- PlatformIO itself: `python -m pip install --user platformio`, then `python -m platformio run` (the `pio` command is often not on the PATH on Windows). The first build downloads the toolchain (several hundred MB).
- Platform `espressif32@6.13.0` (arduino-esp32 2.x) with the maintained `esp32async/ESPAsyncWebServer@^3.12.1` and its AsyncTCP. Both come from the registry, no patches. The old stack (`espressif32@3.5.0` with ESP Async WebServer 1.2.3) crashed the 2.2 firmware on a real device about every 75 seconds with `assertion "new_rcv_ann_wnd <= 0xffff"` in lwIP (`tcp_update_rcv_ann_wnd`), which is why 2.2 was withdrawn once. The new stack fixed it, checked on a device for 5+ minutes.
- Differences of the new library that matter: the `/events` route answers 404 unless the request has `Accept: text/event-stream` (browsers send it, `curl` and scripts must add it), and the manual upload handler is the usual `onUpload` callback.
- The Adafruit Fingerprint library does not define the colors 4-7 of newer sensors. `FingerprintManager.h` defines `FINGERPRINT_LED_GREEN`/`YELLOW`/`CYAN`/`WHITE` itself (`#ifndef`), keep that.
- **Size budget:** the app partition holds 1,310,720 bytes. The firmware currently uses about 89 % (2.2: 1,169,333 bytes). Check the `Flash:` line after every build and do not add large dependencies without removing something. To see what takes the space, link with `-Wl,-Map,out.map` and sum the sections per archive.
- **Filesystem image:** SPIFFS partition at `0x290000`, 1,507,328 bytes (the image is always full size). Firmware at `0x10000`.
- Never change the partition table: devices in the field are updated over the air, which cannot move partitions. `partitions.csv` (set with `board_build.partitions`) reproduces the table of the old platform exactly: app0 `0x10000` and app1 `0x150000` (1280 KB each), SPIFFS `0x290000` (0x170000). The default table of the new platform has a smaller SPIFFS and a coredump partition and must not be used. After a platform change compare `.pio/build/*/partitions.bin` with `python .../tools/gen_esp32part.py`.

## Versioning, releases and the one-click update

- The version string is `VersionInfo` in `src/main.cpp` (format `2.2 Grrzzz`). The web interface compares it with the latest GitHub release tag (`v2.2`, `isNewer()` in `app.js`).
- **Dev and modified builds** have no clean version (anything not starting with `<major>.<minor>`, for example `Develop: develop @ 1a2b3c4`, or a version ending in `+`). `isNewer()` always offers the official release to them, so a test build can always get back onto a release. Test it with `python tools/mockdevice.py 8090 "Develop: develop @ 1a2b3c4"`.
- A release `vX.Y` has one asset for the device, **`update.zip`**, containing `spiffs.bin` first and `firmware.bin` second, both **stored** (not compressed, the device unpacks the stream while receiving it and has no inflate code). Attach `firmware.bin` and `spiffs.bin` as well for flashing with esptool. The device downloads `https://github.com/Grrzzz/FingerprintDoorbell/releases/download/<tag>/update.zip` (`src/SelfUpdate.cpp`), so the name and tag format (`v` followed by digits and dots) must not change.
- `src/GithubRoots.h` holds the root certificates (USERTrust ECC for github.com, ISRG Root X1 for the release CDN). If GitHub changes its chain the update fails with a connection error and the certificates have to be replaced. The manual upload keeps working.
- Update order is **filesystem first, then firmware**, in one stream. A failed run can simply be repeated. The ZIP reader refuses compressed entries, data descriptors and a wrong order with a clear message.

### Creating a release (only when the owner asks)

1. Set `VersionInfo` in `src/main.cpp` to `X.Y Grrzzz` and bump the cache parameter in `data/app.html` if the web interface changed. **The version must be changed before building**, an old build still carries the old string and would offer itself as an update forever.
2. Update `README.md` (what is new, screenshots from the mock) and commit everything. Do not build from a dirty tree.
3. Build: check first that PlatformIO and, for publishing, the `gh` CLI or a browser session are available; without them only steps 1 and 2 can be done, say so instead of tagging an unbuilt commit. Then `pio run`, `touch`/save `src/main.cpp` if the version looks stale, then `pio run -t buildfs`.
4. `python tools/make_update_zip.py vX.Y` checks that `VersionInfo` matches the tag and is inside `firmware.bin`, builds `update.zip` and prints the SHA-256 of all three files.
5. Tag `vX.Y` (annotated) on the commit that was built. A commit after the build makes the binary differ from the tag.
6. After the owner's OK: push the release branch (the remote currently has only `master`; feature branches are merged into it first, ask the owner) and the tag, create the GitHub release (title `X.Y Grrzzz`) with `update.zip`, `firmware.bin`, `spiffs.bin`. The device asks `releases/latest`, so do not mark a test release as latest.
7. Release notes (English): "What is new", "Installation" (update on the Update page, or upload `update.zip` there; first install or recovery: esptool with `bootloader.zip`, `firmware.bin`, `spiffs.bin`), the SHA-256 of the three files, and the line that the release is built and checked but **not a full device test** if that is true.
8. If a tag has to move after publishing: `git push --force origin vX.Y` and replace the assets with the rebuilt files.

### Migration from the old updater

Firmware before this change contains ElegantOTA at `/update` and downloads `firmware.bin`/`spiffs.bin` itself. The new firmware has no `/update` any more. A device on old firmware gets onto the new version once through the old way: either its one-click update (it downloads `firmware.bin` and `spiffs.bin`, so the first release with `update.zip` must also carry both files), or ElegantOTA with `firmware.bin` first and `spiffs.bin` as `filesystem` second. From then on `update.zip` is used.

## Manual update and flashing (for testing on a device)

The Update page has a file picker for `update.zip` (or a plain `firmware.bin`). The same works with curl:

```
curl -F "file=@update.zip" http://<device>/api/update/upload
```

The device unpacks the stream, installs the filesystem and then the firmware, answers `{"ok":true}` and restarts (about 30 s). This is the only `POST` endpoint of the firmware, it exists because it carries a file. A single `firmware.bin` or `spiffs.bin` also works (type detected from the file name, a `.bin` with another name is taken as firmware if it starts with 0xE9); it installs only that part. **Do not flash devices that are not yours or production devices without the owner's approval.**
## Web interface rules (`data/`)

- Vanilla JS in one IIFE (`app.js`), DOM built with the `el(tag, cls, text)` helper, no `innerHTML` with data from the device (names are user input).
- After any change to `app.js` or `style.css` bump the cache parameter in `app.html` **and** `wificonfig.html` (`/app.js?v=N`, `/style.css?v=N`), otherwise browsers and the device keep serving the old files.
- The look follows the owner's AI-on-the-Edge fork (Grrzzz/AI-on-the-edge-device, `sd-card/html/`). Sidebar, footer and the System submenu (Info, Setup, Update, Reboot) are copied 1:1 from there; when changing them, compare with the reference instead of inventing new styles. `wificonfig.html` uses the same `style.css`, keep its classes working.
- Setup (`#view-system-setup`, class `.cfg`) has sections General, Network, MQTT and Factory reset (`data-section`). Rows are shown or hidden with `data-show="mqtt"`, `"mqtt auth"`, `"mqtt ha"` and similar; extend that mechanism rather than toggling single elements. Dialogs that use `.cfg` styles (the Wi-Fi scan `#dlgScan`) must sit inside `.cfg`.
- Network save order: the UI calls `/network?...&noReboot=1` first and then `/settings`, which restarts once for both. The password field shows `********`, sending that back keeps the stored password.
- SSIDs from the Wi-Fi scan come from the air: set them with `textContent` only, like any other device data.
- MQTT status values `disabled` and `unconfigured` are shown as inactive (not as an error) on Overview and Info.
- Light and dark theme come from CSS variables in `style.css`; theme is `auto|light|dark` (`data-theme` on `<html>`, stored in `localStorage`). Use the variables, never fixed colors for text/background.
- Text is English, sentence case, no terminal punctuation on labels.
- Changes of settings use one save bar (appears on System, Setup when something is dirty, *Save & restart* when a restart is needed). Switches and LED settings apply instantly through the API instead.
- Data from the API: fingerprint windows arrive compact as `[days, from, to, allDay]` and are converted by `normalizeFingers()`. Every place that assigns `fingers` must go through it.
- Keep the page working without internet. The release check is the only external request and must stay optional (it runs in the browser, not on the device).

## HTTP API conventions (`src/main.cpp`)

All endpoints are `GET` (except the file upload `POST /api/update/upload`), answer JSON via `sendJson()` and have no authentication (the device is meant for a trusted network). `/api/status`, `/api/fingerprints`, `/api/settings`, `/api/led`, `/api/control`, `/api/finger/policy`, `/api/storage`, `/api/enroll/cancel`, `/api/selfupdate/start`, `/api/wifi/scan`, `/network` are listed in the README. `/api/wifi/scan` is asynchronous (`WiFi.scanNetworks(true)`, answers `running` and `networks`, the UI polls); never block the web server with a synchronous scan. `/network` validates the SSID and the static IP addresses and answers 400 on errors; `noReboot=1` only saves. The setup portal's `/save` (`wificonfig.html`) always resets `staticIp` to false: it is the rescue path when a static address is wrong, keep it. Static files and the SPA are served with `Cache-Control: no-cache`. The old template placeholders (`processor()`) are only used by `wificonfig.html`.

## Fingerprints, schedules and storage

- A fingerprint has a name, a manual switch (`enabled`) and an optional schedule: up to 4 windows (`days` bit 0 = Monday, `from`/`to` minutes since midnight, `allDay`). **With a schedule the schedule decides**, the switch is ignored; without a valid device time a scheduled fingerprint is inactive. A window with `to < from` crosses midnight and belongs to its start day.
- A recognized but inactive fingerprint is handled like an unknown finger in `scanFingerprint()` (the bell rings, no match is published) and reported as **Disabled Match** over MQTT (`disabledMatchName`, `disabledMatchInfo`).
- **Storage constraint (important):** the NVS partition is 20 KB, about 500 usable entries of 32 bytes, shared with Wi-Fi/PHY data and the settings. Names and policies of all 200 fingerprints therefore live in **8 blobs of 25 fingerprints** (`fingers/g0`..`g7`, see `persistGroup()`/`loadFingerListFromPrefs()` in `FingerprintManager.cpp`). Do not store one key per fingerprint, it does not fit. Names are limited to 48 bytes (`limitName()`). `persistGroup()` fails visibly when the area is full, callers must handle that.
- Older versions stored one key per fingerprint in the namespaces `fingerList` and `fingerPolicy`; `migrateLegacyStorage()` converts that once and clears the old keys only after the new ones were written. Do not remove it.
- The System page shows the usage (`/api/storage` parses the raw NVS partition).

## LED ring

Five configurable states (`touchRing`=ready, `scan`, `match`, `ignore`, `gongOff`), each with color (1 red, 2 blue, 3 purple, 4 green, 5 yellow, 6 cyan, 7 white) and sequence (1 breathing, 2 flashing, 3 on, 4 off). Priority when several apply: gong disabled over touch ring ignored over ready (`setLedRingReady()`). Setting names (`<key>Color`, `<key>Sequence`) are used by the API, MQTT (`.../set`) and Home Assistant selects; the table is `ledSlots` in `main.cpp`. Colors 4-7 need newer R503 sensors.

## MQTT and Home Assistant

- Entities are one table (`entities[]`) in `HomeAssistantDiscovery.cpp`. To add one: add a row, publish its state topic (retained for state, not for events), and handle its command topic in `mqttCallback()` in `main.cpp` (subscribe in `connectMqttClient()`).
- Availability is `<root>/connection` (`online`, last will `offline`). Discovery configs are retained under `<prefix>/<component>/fingerprintdoorbell_<mac>/<object>/config`; changing a `unique_id` creates a new entity in Home Assistant, avoid it.
- Event-like values (`disabledMatchName`, `ring`) are not retained, otherwise Home Assistant would re-trigger on restart.

## Settings (`SettingsManager`)

Add a field to the struct, load it with a default in `loadAppSettings()`, write it in `saveAppSettings()` (NVS key names are at most 15 characters), expose it in `getSettingsJson()` and, if needed, accept it in the `/settings` route. Keep defaults compatible with existing devices (never rename or drop existing keys). Settings that need a restart are saved through the Setup save bar, the rest through their own API calls. Example of a compatible default: `mqttEnabled` defaults to "a broker is set", so devices updated from older versions keep MQTT on. Wi-Fi settings hold `staticIp`, `ip`, `subnet`, `gateway`, `dns`; a static configuration is only applied when all of IP, subnet and gateway parse.

## Testing

There are no automated tests. What to do instead:

- Always build (`pio run` and `pio run -t buildfs`) and look at the size.
- For UI work use the virtual device, no hardware needed: `python tools/mockdevice.py 8090` serves `data/` with made-up API data, open `http://127.0.0.1:8090/`. Demo states: `?demo=enroll|schedule|picker|hover|update`, `?theme=dark`, `?hash=fingerprints`. The mock also simulates the Wi-Fi scan (`WIFI_SCAN`, including a hidden SSID and one with HTML in its name), `/network` and restarts with realistic delays. Restart it after editing `mockdevice.py`.
- Browser automation notes: pages with a password field may refuse screenshots, so check states through the DOM. Switches are hidden checkboxes; focus them and press Space instead of clicking.
- On Windows the shell is PowerShell 5: no `&&` (use `;`), write CRLF explicitly (`` "`r`n" ``), and `[IO.File]` methods need absolute paths (`(Resolve-Path f).Path`). Check `app.js` for syntax errors with `node -e "new Function(require('fs').readFileSync('data/app.js','utf8'))"`.
- `python tools/shoot.py 8090 shots` creates the README screenshots with headless Edge (needs Pillow and Edge). Screenshots in the README must only show made-up data, never a real device, names, addresses or credentials.
- Changes that touch storage, MQTT or the scan flow need a real device. Test them on a device that does not hold real data first, a migration or a changed storage layout can lose names and schedules.

## Pitfalls collected the hard way

- Heap is tight during TLS (the self-update): do not add allocations to the update path, and keep the web server idle logic out of `loop()` while `selfUpdateRunning()`.
- The scan loop blocks inside `enrollFinger()`; anything that must end an enrollment (cancel, reboot) has to be checked inside its loops (`enrollAborted()`).
- `getFingerListAsJson()` builds one large String (up to about 40 KB for 200 fingerprints); keep the per-fingerprint JSON small and the `reserve()` call.
- The sensor can be missing (`fingerManager.connected == false`): list and count functions then return empty values, keep it that way.
- Files with Windows line endings are normal in this repository, do not mass-convert them.
- Secrets (GitHub tokens, Wi-Fi or MQTT credentials) must never be committed or pasted into issues, pull requests or release notes.

## Commit and pull request notes

Small focused commits with an imperative subject line. Describe the user-visible effect and anything that needs a device test. Update `README.md` (and the screenshots, via the virtual device) when behaviour visible to users changes.
