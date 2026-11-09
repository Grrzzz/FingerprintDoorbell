# AGENTS.md

Guide for AI coding agents (and humans) working on this repository. It is the fork **Grrzzz/FingerprintDoorbell** of frickelzeugs/FingerprintDoorbell: an ESP32 + Grow R503 fingerprint reader that doubles as a doorbell, with a web interface, MQTT and Home Assistant auto discovery. `README.md` describes the features for users, this file explains how to change the code without breaking it.

## Stack at a glance

- ESP32 (`esp32doit-devkit-v1`), Arduino framework, PlatformIO (`espressif32@3.5.0`, arduino-esp32 1.0.6). C++11 style of the Arduino core, no exceptions.
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
| `src/SelfUpdate.*`, `src/GithubRoots.h` | one-click update (HTTPS download from GitHub releases) and its root certificates |
| `data/` | web interface: `app.html`, `app.js`, `style.css`, plus `wificonfig.html` (Wi-Fi setup mode, still templated by the server) |
| `lib/` | libraries that are installed manually (the registry no longer resolves ESP Async WebServer), only a README is tracked |
| `lib_patches/` | patches that must be applied to those libraries |
| `tools/` | virtual device and screenshot scripts for working on the UI without hardware |
| `doc/` | images for the README, `bootloader.zip` for esptool flashing |

## Build

```
pio run                  # builds firmware.bin
pio run -t buildfs       # builds spiffs.bin from data/ (web interface)
```

- Install the libraries from `lib_deps` in `platformio.ini` into `lib/` if the registry cannot resolve them, then apply `lib_patches/ESPAsyncWebServer-no-regex.patch` inside `lib/ESP Async WebServer` (`git apply`). Without it `std::regex` is linked in and the firmware is about 180 KB larger, which no longer fits next to the one-click update.
- **Size budget:** the app partition holds 1,310,720 bytes. The firmware currently uses about 90 %. Check the `Flash:` line after every build and do not add large dependencies without removing something. To see what takes the space, link with `-Wl,-Map,out.map` and sum the sections per archive.
- **Filesystem image:** SPIFFS partition at `0x290000`, 1,507,328 bytes (the image is always full size). Firmware at `0x10000`.
- Never change the partition table: devices in the field are updated over the air, which cannot move partitions.

## Versioning, releases and the one-click update

- The version string is `VersionInfo` in `src/main.cpp` (format `2.1 Grrzzz`). The web interface compares it with the latest GitHub release tag (`v2.1`).
- A release (`vX.Y`) needs exactly two assets named `firmware.bin` and `spiffs.bin` (from `.pio/build/esp32doit-devkit-v1/`). The device downloads `https://github.com/Grrzzz/FingerprintDoorbell/releases/download/<tag>/firmware.bin` and `.../spiffs.bin` itself (`src/SelfUpdate.cpp`), so names and tag format (`v` followed by digits and dots) must not change.
- Release notes go into the GitHub release, in English, with the SHA-256 of both files and a short manual update guide (firmware first, then filesystem).
- `src/GithubRoots.h` holds the root certificates (USERTrust ECC for github.com, ISRG Root X1 for the release CDN). If GitHub changes its chain the update fails with a connection error and the certificates have to be replaced. The manual updater at `/update` keeps working.
- Update order in the self-update is **filesystem first, then firmware**, after both files were checked. A failed run can simply be repeated.

## Manual update and flashing (for testing on a device)

The device exposes ElegantOTA at `/update`:

```
curl -F "MD5=<md5 of file>" -F "file=@firmware.bin;filename=firmware.bin" http://<device>/update
cp spiffs.bin filesystem
curl -F "MD5=<md5 of file>" -F "file=@filesystem;filename=filesystem" http://<device>/update
```

The `MD5` field must come first, a file whose name is `filesystem` goes to SPIFFS. Flash `firmware.bin` first, wait for the restart (about 30 s), then `spiffs.bin`. The endpoint sometimes answers `400` right after a reboot, wait a few seconds and repeat. **Do not flash devices that are not yours or production devices without the owner's approval.**

## Web interface rules (`data/`)

- Vanilla JS in one IIFE (`app.js`), DOM built with the `el(tag, cls, text)` helper, no `innerHTML` with data from the device (names are user input).
- After any change to `app.js` or `style.css` bump the cache parameter in `app.html` (`/app.js?v=N`, `/style.css?v=N`), otherwise browsers and the device keep serving the old files.
- Light and dark theme come from CSS variables in `style.css`; theme is `auto|light|dark` (`data-theme` on `<html>`, stored in `localStorage`). Use the variables, never fixed colors for text/background.
- Text is English, sentence case, no terminal punctuation on labels.
- Changes of settings use one save bar (appears on System and Connectivity when something is dirty). Switches and LED settings apply instantly through the API instead.
- Data from the API: fingerprint windows arrive compact as `[days, from, to, allDay]` and are converted by `normalizeFingers()`. Every place that assigns `fingers` must go through it.
- Keep the page working without internet. The release check is the only external request and must stay optional (it runs in the browser, not on the device).

## HTTP API conventions (`src/main.cpp`)

All endpoints are `GET`, answer JSON via `sendJson()` and have no authentication (the device is meant for a trusted network). `/api/status`, `/api/fingerprints`, `/api/settings`, `/api/led`, `/api/control`, `/api/finger/policy`, `/api/storage`, `/api/enroll/cancel`, `/api/selfupdate/start` are listed in the README. Static files and the SPA are served with `Cache-Control: no-cache`. The old template placeholders (`processor()`) are only used by `wificonfig.html`.

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

Add a field to the struct, load it with a default in `loadAppSettings()`, write it in `saveAppSettings()` (NVS key names are at most 15 characters), expose it in `getSettingsJson()` and, if needed, accept it in the `/settings` route. Keep defaults compatible with existing devices (never rename or drop existing keys). Settings that need a restart are saved through the Connectivity/System save bar, the rest through their own API calls.

## Testing

There are no automated tests. What to do instead:

- Always build (`pio run` and `pio run -t buildfs`) and look at the size.
- For UI work use the virtual device, no hardware needed: `python tools/mockdevice.py 8090` serves `data/` with made-up API data, open `http://127.0.0.1:8090/`. Demo states: `?demo=enroll|schedule|picker|hover|update`, `?theme=dark`, `?hash=fingerprints`.
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
