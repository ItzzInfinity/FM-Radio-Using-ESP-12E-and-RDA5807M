# FM Radio Product Architecture

**Project:** RDA5807M FM Radio with ESP8266 ESP-12E / ESP32-C3, browser UI, and Android companion app  
**Architecture status:** Proposed target architecture around the existing `test_I2C_Radio.ino` sketch  
**Primary delivery model:** Radio joins the owner's router or phone hotspot for everyday use. It creates its own Wi-Fi access point for first-time setup and as a fallback, so it can always be controlled without a router or internet connection.  
**Revision 2026-10-29:** Changed from AP-only to AP + STA so units can be shipped to other people (section 1, section 4 "Joining the user's router or phone hotspot"). Added station scanning and a persistent station log (section 4 "Station scan and station log").

---

## 1. Executive decision

Do not replace the radio implementation or rebuild the project from scratch. Keep the working radio, persistence, display, physical button, web page, and OTA upload paths. Add a network-access layer around them and expose a stable API that both the existing browser UI and the Android app use.

### Target behaviour

1. On boot, the radio hardware starts independently of Wi-Fi.
2. If no network is saved, the ESP starts a setup access point (SoftAP), for example `FM-Radio-A1B2C3`, with a captive setup page at **`http://192.168.4.1/`**. The user picks their router or phone hotspot and enters its password.
3. If networks are saved, the ESP joins the first one in range (STA) and is reachable at its DHCP IP and at `http://radio-a1b2c3.local`.
4. If no saved network can be joined within about 30 s, the setup AP comes back up alongside the STA retries. The radio is then reachable at `192.168.4.1` again.
5. The web UI continues to work in a phone browser in every mode.
6. The Android app finds the radio on the shared network (DNS-SD), or joins its AP using Android's supported network-request flow. It verifies the device over HTTP and then displays the control UI.
7. If the phone changes networks, the radio reboots, or a request times out, the app detects the condition and offers a deterministic reconnect path. It does not silently assume that Wi-Fi association means the HTTP API is available.
8. The physical preset button and radio audio keep working if Wi-Fi or the phone is unavailable.

### First release networking decision

*Revised 2026-10-29. This replaces the earlier AP-only decision.*

Use **AP + STA with provisioning** for the first release. Units go to other people, and they expect the radio on their home Wi-Fi or phone hotspot, so neither build-time credentials (`secrets.h`) nor AP-only operation is enough. The AP remains the setup and recovery path. Basic control must never depend on a router being present. In STA mode the DHCP IP (shown on the OLED) and DNS-SD discovery are the canonical addresses, and `.local` is a convenience. In AP mode `192.168.4.1` is canonical.

---

## 2. What already exists and must be preserved

The current sketch is already a dual-target implementation for ESP8266 ESP-12E and ESP32-C3. It includes an RDA5807M driver, settings with CRC validation and delayed flash writes, ten editable presets, a hardware button, optional SSD1306/SH1106 OLED, a self-contained browser UI, JSON status, and authenticated browser OTA. The sketch currently starts as a Wi-Fi station and waits for a router, which is the key network behaviour to change. See the source header and implementation sections for the existing feature list and target split.

### Existing firmware responsibilities

| Existing component | Current responsibility | Architecture treatment |
|---|---|---|
| `RDA5807M` | I²C radio initialization, tune, volume, seek, RSSI/stereo/tuned state | Keep as the hardware driver. Do not expose registers to the app. |
| `Settings` / `settingsLoad()` / `settingsSaveNow()` | Frequency, preset selection, volume, OLED choice, preset names/frequencies, CRC | Keep. Add network provisioning separately or with an explicit schema migration; do not silently change the persisted struct. |
| `applyFrequency()` / `applyPreset()` / `applyVolume()` | Application-level radio state changes and persistence scheduling | Keep as the single control path. New API handlers should call these functions rather than duplicate the logic. |
| `oledBegin()` / `oledRender()` | Optional local display | Keep. Change the network footer to show AP IP/connection status correctly. |
| `buttonTick()` / `nextPreset()` | Local preset control | Keep independent of network state. |
| `INDEX_HTML` and `/api/status` | Browser UI and status polling | Keep in the first incremental implementation. Migrate the UI and app onto a versioned API without breaking existing routes during transition. |
| `/update` | Password-protected browser firmware upload | Keep available locally, but treat it as an admin function and never expose a shared production password. |
| `wifiBegin()` / `networkTick()` | Currently joins the router hard-coded in `secrets.h`, starts mDNS and Arduino OTA after association, and retries that one network forever | Replace with the provisioning state machine in section 4: saved networks from flash, AP fallback, and mDNS/OTA started in both modes. |
| `RDA5807M::seek()` + `/api/seek` | Blocking hardware seek. The result is applied as `PRESET_MANUAL` and never stored, except as the last tuned frequency. | Keep the driver primitive. Add a non-blocking scan state machine and a station log (section 4). |
| `loop()` | Services HTTP, OTA, mDNS, button, persistence and radio status | Preserve its non-blocking shape; avoid adding long waits to app request handling. |

### Important code-level implications

- The current `wifiUp()` returns `WiFi.status() == WL_CONNECTED`. That is suitable for the current station-mode sketch, but it is **not an AP-running test**. In AP-only mode it can be false while the SoftAP and web server are working.
- `buildStatusJson()` currently reads `WiFi.localIP()`, which is the station interface address. AP-only status must use `WiFi.softAPIP()` and report the active network mode.
- The OLED footer currently displays the station IP or “Wi-Fi: connecting...”. In AP-only mode it should display the AP address, normally `192.168.4.1`.
- The web page displays `http://radio.local` as a hostname hint. Keep this as optional information but always display `http://192.168.4.1` as the primary AP-mode address.
- The current `/api/*` controls mutate state using HTTP GET. That can remain temporarily for compatibility, but the new app-facing API should use GET for reads and POST/PUT for mutations. Do not cache mutation requests.
- A seek can wait up to about three seconds for the RDA5807M status flag. The app must not treat a slow seek as an immediate device disconnect. Prefer a non-blocking seek job in the hardening phase.

---

## 3. High-level system architecture

```text
                         ┌───────────────────────────────┐
                         │       Android companion      │
                         │                               │
                         │ Compose UI                    │
                         │ ViewModel / UI state          │
                         │ Device connection coordinator  │
                         │ Radio API client               │
                         │ Pairing record / QR parser    │
                         └──────────────┬────────────────┘
                                        │ HTTP over Wi-Fi
                                        │ Network-bound requests
                                        ▼
┌─────────────────────────────────────────────────────────────────────┐
│                         ESP SoftAP network                          │
│  SSID: FM-Radio-<unit-id>     IP: 192.168.4.1                       │
│                                                                     │
│  ┌────────────────┐   ┌─────────────────────┐   ┌────────────────┐ │
│  │ HTTP server    │──▶│ API / command layer  │──▶│ Radio state    │ │
│  │ Browser UI     │   │ validation/errors    │   │ controller     │ │
│  │ OTA page       │   │ compatibility routes │   │ presets/volume │ │
│  └────────────────┘   └─────────────────────┘   └───────┬────────┘ │
│                                                         │          │
│                       ┌─────────────────────┐           │          │
│                       │ Settings persistence│           │          │
│                       │ CRC / debounce save │           │          │
│                       └─────────────────────┘           │          │
│                                                         ▼          │
│                    ┌────────────────────┐  ┌────────────────────┐ │
│                    │ RDA5807M I²C driver │  │ OLED + button      │ │
│                    └────────────────────┘  └────────────────────┘ │
└─────────────────────────────────────────────────────────────────────┘
```

### Architectural rule

**The browser and Android app are clients of the same API.** Neither client accesses I²C registers, EEPROM/Preferences, or radio-driver functions directly. The app must not have its own separate radio-state logic; the ESP firmware is the source of truth for frequency, volume, preset selection, signal status, and display configuration.

### Planes

- **Control plane:** Wi-Fi association, device identity, API health, reconnects, HTTP requests, error reporting.
- **Radio plane:** RDA5807M tune/seek/volume and current status.
- **Persistence plane:** Existing settings structure, CRC checks, and deferred writes.
- **Local fallback plane:** Physical button and optional OLED. This plane continues even when no phone is connected.
- **Maintenance plane:** Authenticated firmware update endpoint and firmware/build identity.

---

## 4. Firmware architecture

The first change can stay in the current single `.ino` file. Once AP mode and the app work, the logical components can be split into `.h/.cpp` files without changing their contracts.

```text
Firmware
├── BoardProfile
│   ├── ESP8266 / ESP-12E pin map and core shims
│   └── ESP32-C3 pin map and core shims
├── RadioDriver
│   └── RDA5807M I²C register access, tune, seek, volume, status
├── RadioController
│   ├── applyFrequency / applyPreset / applyVolume
│   ├── current station and preset state
│   └── validation and command result
├── SettingsStore
│   ├── current Settings struct
│   ├── CRC + version validation
│   └── delayed persistence
├── DisplayService
│   └── SSD1306 / SH1106 / no display
├── ButtonService
│   └── debounce and next preset
├── NetworkManager        [new/modified]
│   ├── AP identity and credentials
│   ├── SoftAP startup and IP configuration
│   ├── AP health and client count
│   └── optional mDNS / future AP+STA policy
├── ApiRouter             [modified]
│   ├── health and info
│   ├── status and commands
│   ├── validation / standard errors
│   └── old-route compatibility during migration
├── WebUi                 [existing embedded HTML]
└── OtaService            [existing, restricted]
```

### Startup sequence

1. Initialize serial diagnostics and the board button pin.
2. Initialize I²C and probe the bus.
3. Load and validate radio settings; fall back to factory defaults on invalid data.
4. Initialize the RDA5807M and apply stored frequency and volume if present.
5. Initialize the optional OLED and render the current radio state.
6. Initialize the SoftAP with the per-device SSID and WPA2 credential; configure the AP IP/subnet.
7. Register HTTP routes and start the web server regardless of whether the RDA5807M was found.
8. Start optional mDNS if it works in the current build, but do not make it a prerequisite for local access.
9. Continue servicing HTTP, button debounce, settings-save timer, radio status refresh, and OLED rendering from `loop()`.

If the radio IC is absent, the device should still expose `/api/v1/health`, `/api/v1/info`, and a useful web error state. Network service availability must not depend on a successful I²C probe.

### Network manager contract

Use explicit helpers instead of one overloaded `wifiUp()` meaning:

- `apStarted()` — the SoftAP interface has been configured and started.
- `apIp()` — returns `WiFi.softAPIP()`.
- `apClientCount()` — reports connected Wi-Fi stations if the selected core supports it.
- `networkServiceReady()` — AP is started and HTTP server initialization completed.
- `staConnected()` — joined to one of the saved networks (`WiFi.status() == WL_CONNECTED`).
- `netMode()` — `SETUP_AP`, `STA_CONNECTING`, `STA`, or `STA_AP_FALLBACK`. Reported in status JSON and on the OLED.

`networkTick()` must **not** call `WiFi.begin(WIFI_SSID, WIFI_PASSWORD)` with build-time credentials. It must also not disconnect and reassociate in a tight loop, because each scan or retry briefly takes the AP channel away from phones connected to it. It drives the state machine below.

### AP network settings

- SSID: `FM-Radio-<last 6 hex characters of the unit identifier>`.
- IP address: `192.168.4.1`.
- Gateway: `192.168.4.1`.
- Subnet: `255.255.255.0`.
- HTTP port: `80`.
- AP mode: WPA2-Personal, with a unique per-device password.
- Station count: choose a deliberate cap (for example, 4). Keep expectations modest on ESP8266.
- Internet: not provided; this is a local control network.

The ESP8266 Arduino core exposes `WiFi.softAP(...)` and `WiFi.softAPConfig(...)`; use the matching Arduino-core methods for ESP32-C3 as well, then compile-test both targets. See the official [ESP8266 Soft Access Point API](https://github.com/esp8266/Arduino/blob/master/doc/esp8266wifi/soft-access-point-class.rst).

### Credentials and provisioning

Do **not** ship every retail unit with one shared password embedded in public firmware. A MAC address or SSID suffix is an identifier, not a secret.

Recommended production flow:

1. Generate a random per-unit WPA2 passphrase in a manufacturing/provisioning step.
2. Store the passphrase and unit identifier in a dedicated provisioning record or inject them during per-unit provisioning.
3. Print a QR code and human-readable SSID/password on the unit's label or documentation.
4. QR payload contains a schema version, device ID, SSID, password, and AP base URL/IP. Do not put secrets in a public source repository.
5. Let the user factory-reset network credentials through a deliberate documented procedure. Do not let a casual radio button press reset security credentials accidentally.

For a bench prototype, a local `secrets.h` credential is acceptable. It is not the production provisioning strategy. Keep OTA credentials separate from Wi-Fi AP credentials.

### Persistence and schema rules

The existing `Settings` struct is the radio user's configuration and already has a magic, version, and CRC. Do not casually append network fields to it without increasing the version and defining migration behaviour. Prefer a separate `ProvisioningRecord` with its own version/CRC, or platform-specific persistent storage behind a common interface. A firmware update must preserve the user's presets and radio state.

### Joining the user's router or phone hotspot (AP + STA)

*Added 2026-10-29.* The current firmware can only join the network compiled in from `secrets.h`. A shipped unit has to learn its owner's network at runtime.

#### Ways to provision Wi-Fi credentials

| Option | Targets | Effort | Notes |
|---|---|---|---|
| **A. Captive setup page on the radio's AP** | Both | Low–medium | **Release 1 baseline.** On first boot, or when no saved network is reachable, start `FM-Radio-XXXXXX` with `DNSServer` (both cores) answering every name with `192.168.4.1`. `/setup` lists `WiFi.scanNetworks()` results. The user picks one and types the password, and the radio tests the join before saving. Write it in-tree with the existing web server, which matches the project's no-external-library style. tzapu WiFiManager is a drop-in alternative for both cores if speed matters more than control. |
| **B. Android app sends credentials** | Both | Medium | The app joins the AP (`WifiNetworkSpecifier`, section 6) and calls `PUT /api/v1/wifi`. This fits the existing QR pairing. Build it after A, on the same endpoint. |
| **C. Improv Wi-Fi over serial (ESP Web Tools)** | Both | Low | For hobbyists flashing a released `.bin` from a browser: flash and Wi-Fi setup happen on one page. |
| D. BLE provisioning (Improv-BLE / Espressif WiFiProv) | ESP32-C3 only | Medium | The ESP-12E has no BLE, so this means two onboarding flows. Not for release 1. |
| E. WPS | Mostly ESP8266 | Low | Phone hotspots don't support it and many routers disable it. Rejected. |

#### Network state machine

```text
boot ── no saved network ──────────────────────────▶ SETUP_AP
  │                                                   (WIFI_AP, captive DNS, /setup)
  │                                                   │ credentials tested OK → save
  ▼ saved networks                                    ▼
STA_CONNECTING (WIFI_STA, WiFiMulti over saved list) ◀─────────┘
  ├── joined within 30 s ───────▶ STA  (mDNS + DNS-SD + OTA started)
  │                                 └── link lost > 30 s ──▶ STA_AP_FALLBACK
  └── timeout ──────────────────▶ STA_AP_FALLBACK
                                    (WIFI_AP_STA: AP up, STA retry every 60 s;
                                     AP stops 2 min after STA joins and no AP client)
```

- Start mDNS, DNS-SD (`_http._tcp`, which the Android spike already scans for), and Arduino OTA when either interface comes up, not only on the STA edge (see 14.1 item 1).
- In `WIFI_AP_STA` the AP is forced onto the router's channel. A STA scan or retry briefly drops phones connected to the AP, so space retries 60 s apart, not on every `loop()`.
- Never block `loop()` on a join. The button, radio, and settings-save timer keep running.

#### Credential storage

- Keep a separate `NetRecord` with its own magic/version/CRC. Do not put network fields in `Settings`: changing `SETTINGS_VER` currently wipes presets (14.1 item 8).
  - Up to 3 networks: `ssid[33]`, `pass[65]` (≈98 B each), for example home router + phone hotspot + one spare.
  - `otaPass[33]`, set by the owner on the setup page instead of a shared build-time `OTA_PASSWORD`.
- ESP8266: EEPROM layout `Settings` at offset 0 (172 B, with space reserved up to 256), `NetRecord` at offset 256. Every `EEPROM.begin()` call uses the same size, for example 1024.
- ESP32-C3: a separate `Preferences` namespace (`"net"`).
- The SDK's own persistent storage (`WiFi.persistent(true)` plus `WiFi.begin()` with no arguments) remembers only one network. It is fine for a prototype but not enough for "home + hotspot".
- `/api/v1/wifi` and status JSON return saved SSIDs only, never passwords.

#### Reset and identity

- **Wi-Fi reset:** hold the button at power-on for 10 s. This clears `NetRecord` only and keeps presets and the station log. A normal press still cycles presets, so it can't wipe settings by accident.
- **Unique names:** hostname `radio-<last 6 hex of MAC>`, so two radios on one LAN don't both claim `radio.local`. The AP SSID uses the same suffix.

#### Phone hotspot and router notes (quick-start card)

- The ESP8266 and ESP32-C3 are **2.4 GHz only**. On iPhone enable "Maximize Compatibility". On Android set the hotspot band to 2.4 GHz.
- The **ESP8266 has no WPA3**. Use WPA2, or WPA2/WPA3 transition mode.
- Many hotspots and some routers don't pass mDNS, and Android browsers can't resolve `.local` names. Use the IP on the OLED or the app's DNS-SD scan. Units without an OLED depend on the app, or on the router's client list.
- Networks with client isolation (guest Wi-Fi) block phone-to-radio traffic. In that case use the AP fallback.

### Station scan and station log

*Added 2026-10-29.* Today `/api/seek` applies whatever it lands on as `PRESET_MANUAL`. Only the 10 presets and the last tuned frequency survive a reboot. Seek results are never stored.

#### Three levels, smallest first

*Implemented 2026-10-29 (`SEEK_AUTOSAVE`):* `/api/seek` now saves a found station automatically. When the seek ends on a station (STC set, SF clear, FM_TRUE) that isn't already in the list, it is written to the next slot whose name is still its own frequency. Slots fill as a ring starting from `Settings.autoSlot`, the former `pad` byte, so `SETTINGS_VER` is unchanged and existing presets load intact. The reply carries `seekSaved` (slot index, or `-1`).

1. **Save current station to a preset.** No firmware change is needed: `GET /api/preset/set?i=N&freq=<current>&name=…` already accepts any frequency. Add a "Save to preset" button after seek in the Android app, and in `INDEX_HTML`, which is a UI-only change.
2. **Full-band scan into a station log.** Covered below.
3. **Fill presets from the first scan.** `FACTORY_PRESETS` are one city's stations, so a shipped unit elsewhere starts with mostly empty slots. After the first scan, or after a factory reset, fill the 10 presets with the strongest results, but only if the presets are untouched (equal to `FACTORY_PRESETS` with default names). That check needs no `Settings` change. A long button press can start a rescan.

#### Scan state machine (non-blocking)

`seek()` blocks for up to 3 s (`waitForSTC(3000)`), so a scan written as a loop of seeks would freeze HTTP for 20–40 s. Instead, `scanTick()` runs from `loop()`:

```text
IDLE ─start─▶ TUNE 87.0 MHz ─▶ SEEKING (write SEEK|SEEKUP|SKMODE, return)
                                  │ each loop(): read 0x0A; STC set?
                                  ▼
                                LANDED ── SF set (band edge) ─────────────▶ COMMIT
                                  │ FM_TRUE && RSSI ≥ threshold → keep in RAM batch
                                  │ optional: wait ≤ 2.5 s for RDS PS name
                                  └─▶ SEEKING (next)          timeout 4 s/step → FAILED
COMMIT: merge batch into log (one flash write per scan) → retune previous station → IDLE
```

- A `scanInProgress` guard rejects tune/seek/preset commands with `BUSY`. Status JSON reports progress (current frequency, found count).
- The RAM batch is at most 211 entries. The band 87.0–108.0 MHz at 0.1 MHz steps (`FM_BAND_LOW10K`…`FM_BAND_HIGH10K`) has exactly **211 channels**, so one scan can never find more.
- RDS PS names: `RDA_RDS_EN` is already defined. Reading the 8-character PS name per station adds about 2–3 s per station, so make it optional.

#### Station record (24 bytes, size checked with the ESP8266 compiler)

```c
struct StationRecord {          // sizeof == 24 on ESP8266, no packing needed
  uint16_t freq10k;             // 8700..10800
  uint8_t  rssi;                // 0..127, strongest seen
  uint8_t  flags;               // stereo | rds_ok | loc_valid | favourite | hidden
  char     ps[8];               // RDS programme service name, not NUL-terminated
  int32_t  latE5;               // degrees × 1e5 (~1 m), from the phone; 0 if !loc_valid
  int32_t  lonE5;
  uint32_t lastSeen;            // unix time from NTP while on STA, 0 if unknown
};
```

**A location is required to keep a log across a country.** Frequencies repeat from city to city (the same 93.5 MHz is a different station in each one), and there are only 211 channels. A frequency-only log saturates at 211 entries and loses which city a station belongs to. The board has no GPS, so the app sends the phone's location with the scan request. A button-started scan stores `loc_valid = 0`.

**Duplicate rule on commit:** treat a result as the same station when `freq10k` matches and either both locations are valid and less than 25 km apart, or neither is valid. Update `rssi`/`ps`/`lastSeen` in place. Otherwise append. Do one sequential pass over the file per scan, comparing every record against the RAM batch, not one pass per station.

#### Where the log lives: ESP8266 (ESP-12E, 4 MB) capacity

The log goes in a **LittleFS file `/stations.bin`** (16-byte header + fixed 24-byte records, count = `(size − 16) / 24`). Do not put it in EEPROM emulation. The default FQBN `esp8266:esp8266:nodemcuv2` uses `eesz=4M2M`: FS `0x200000–0x3FA000` = 2,072,576 B = 253 × 8 KiB blocks, with OTA room of about 1019 KB unchanged.

Capacity was measured with the core's own `mklittlefs` 3.1.0 (block 8192, page 256) by finding the largest single file that fits. Runtime figures keep **2 blocks (16 KiB) free** so LittleFS copy-on-write and metadata compaction can still append.

| Storage on ESP8266 | Bytes available for the log | 24 B records (with location) | 4 B records (freq + RSSI + flags) |
|---|---|---|---|
| **Today: `Settings` presets** | — | **10 stations** (`PRESET_COUNT`) | — |
| EEPROM emulation, rest of the 4096 B sector after `EEPROM.begin(sizeof(Settings)+8)` = 180 B, minus an 8 B log header | 3,908 | **162** | 977 |
| LittleFS, `eesz=4M1M` (1,024,000 B FS; max file 1,006,660 B) | 990,260 | **41,260** | 247,565 |
| **LittleFS, `eesz=4M2M` (default; max file 2,054,216 B)** | **2,037,816** | **84,909** | 509,454 |
| Firmware cap (proposed `STATION_LOG_MAX = 65535`, so indexes fit in `uint16_t`) | 1,572,840 (+16 header) | **65,535** | — |

For scale: all of India has roughly 1,200–1,500 FM stations (AIR + private + community, rough estimate). A lifetime log of every one of them is about 36 KB, about 1.8% of the 2 MB filesystem and 2.3% of the 65,535 cap. With the duplicate rule, travel doesn't grow the log past the real number of stations. Flash space is not the limit:

- **RAM:** `jsonBuf` is 1024 B, and globals already use 31,880 of 80,192 B (measured 2026-10-29). Never load the log into RAM. Serve it in pages (`GET /api/v1/stations?offset=&count=50`) with chunked `sendContent()`.
- **Duplicate-check time:** one sequential read of the file per scan. Small at 1–2k records; at the 65k cap it is a 1.5 MB read per scan, so keep the cap.
- **Wear and power loss:** one LittleFS write per completed scan, never per seek. LittleFS is power-loss safe. Derive the record count from the file size, so a torn last record is simply dropped.
- **Things that erase the log:** uploading a LittleFS image from the IDE, and "Erase all flash" both delete it. A sketch-only OTA update keeps it.

ESP32-C3 (4 MB, default partition table): the `spiffs` partition is `0x160000` = 1,441,792 B, and the largest single file is 1,430,832 B (measured the same way, 4 KiB blocks). Keeping 2 blocks free, that is **59,276 records** at 24 B. Mount it with `LittleFS.begin(true)`. NVS (`Preferences`) is too small and too heavy per entry for the log, so keep the log out of it.

#### Display: presets vs log

Presets stay the user's 10 favourites in `Settings`, used by the button and the OLED. The log is everything ever received. The app filters it as "near me" (phone GPS, 25 km), "this scan", or "all", and offers "Save to preset N".

---

## 5. HTTP API contract

### Addressing and compatibility

Canonical AP-mode base URL: `http://192.168.4.1`  
Browser UI: `GET /`  
Firmware identity: `GET /api/v1/info`  
Health check: `GET /api/v1/health`  
Current state: `GET /api/v1/status`

During migration, preserve the existing routes so that the current embedded webpage remains functional. New clients should use `/api/v1/*`. Once the new UI and Android app are stable, the old routes can be kept as thin wrappers for backwards compatibility or removed in a documented API-breaking release.

### Version 1 endpoints

| Method | Endpoint | Purpose | Successful response |
|---|---|---|---|
| GET | `/api/v1/health` | Lightweight readiness probe | `200` JSON with `ok`, `device_id`, `api_version`, `uptime_ms` |
| GET | `/api/v1/info` | Identity and capabilities | `200` JSON with `device_type`, `device_id`, `board`, `firmware`, `api_version`, `ip`, `ssid`, feature flags |
| GET | `/api/v1/status` | Complete current radio/UI state | `200` JSON: frequency, preset index, volume, RSSI, stereo, tuned, radio presence, OLED state, preset array |
| POST | `/api/v1/commands/preset` | Select preset | JSON body: `{"index":0}` |
| POST | `/api/v1/commands/tune` | Tune to frequency | JSON body: `{"frequency_10khz":9830}`; frequency unit matches current firmware convention |
| POST | `/api/v1/commands/volume` | Set volume | JSON body: `{"value":13}` |
| POST | `/api/v1/commands/seek` | Seek up/down | JSON body: `{"direction":"up"}`; use a job response if implemented asynchronously |
| PUT | `/api/v1/presets/{index}` | Edit a preset | JSON body: `{"frequency_10khz":9830,"name":"Station"}` |
| PUT | `/api/v1/display` | Set OLED mode | JSON body: `{"type":"off"}` or `ssd1306` / `sh1106` |
| GET | `/api/v1/wifi` | Network mode, current SSID/IP, saved SSIDs (never passwords) | `200` JSON |
| GET | `/api/v1/wifi/scan` | Nearby networks for the setup page/app | `200` JSON: `ssid`, `rssi`, `secure`. Runs async; `202` while scanning |
| PUT | `/api/v1/wifi` | Add or replace a saved network; the radio test-joins before saving | JSON body: `{"ssid":"Home","password":"…"}`; result says joined / wrong password / not found |
| DELETE | `/api/v1/wifi/{index}` | Forget a saved network | `200` with the updated list |
| POST | `/api/v1/scan` | Start a full-band scan | JSON body: `{"lat":17.38500,"lon":78.48670,"rds":false}` (location optional) → `202` |
| GET | `/api/v1/scan` | Scan progress / last result | `state`, `frequency_10khz`, `found`, batch list when done |
| GET | `/api/v1/stations` | Paged station log | `?offset=0&count=50[&lat=&lon=&radius_km=25]`; `total` included |
| DELETE | `/api/v1/stations/{index}` | Hide a log entry (sets the `hidden` flag) | `200` |

The exact JSON keys are the contract: document them once and keep browser and app clients aligned. Use a proper JSON serializer or carefully bounded serialization; never directly concatenate untrusted text into JSON. Preset names must remain capped and validated as they are in the existing firmware.

### Example status JSON

```json
{
  "device_type": "fm-radio",
  "device_id": "A1B2C3",
  "api_version": 1,
  "firmware": "2026-10-25 12:00:00",
  "board": "ESP8266 (ESP-12E)",
  "network": {
    "mode": "ap",
    "ready": true,
    "ip": "192.168.4.1",
    "ssid": "FM-Radio-A1B2C3"
  },
  "radio": {
    "present": true,
    "frequency_10khz": 9830,
    "frequency_mhz": 98.3,
    "preset_index": 4,
    "volume": 13,
    "rssi": 42,
    "stereo": true,
    "tuned": true
  },
  "display": {"type": "ssd1306", "detected": true},
  "presets": [
    {"index": 0, "frequency_10khz": 10620, "name": "106.2"}
  ]
}
```

This is an illustrative contract, not a claim that the current sketch already returns that exact structure. During migration, either keep the existing `/api/status` JSON intact or update the webpage at the same time as the new API.

### Error format

All API errors should use the same shape and a meaningful HTTP status:

```json
{"error":{"code":"INVALID_ARGUMENT","message":"Volume must be between 0 and 15"}}
```

Recommended codes: `INVALID_ARGUMENT`, `NOT_FOUND`, `RADIO_NOT_PRESENT`, `BUSY`, `UNAUTHORIZED`, `INTERNAL_ERROR`. Validate the entire input before changing radio state. Clamp only where the existing product contract intentionally defines clamping; otherwise reject invalid values so the app can tell the user what happened.

### Mutation and request semantics

- GET handlers must not change device state in the new API.
- Return the resulting state or a command identifier after a mutation.
- Include a short client timeout for ordinary reads and a longer timeout for seek/OTA actions.
- A timeout does not prove the command failed: after any write timeout, fetch status before automatically repeating the command.
- Add `Cache-Control: no-store` on API responses, especially status and command responses.
- No CORS configuration is required for a native Android HTTP client. If the web UI remains same-origin, it can use relative paths.
- Keep OTA on its separate authenticated route. Do not include OTA update functionality in the consumer-facing app's first release unless there is a signed/verified firmware-update plan.

### Existing-route mapping

| Existing route | New API equivalent |
|---|---|
| `GET /api/status` | `GET /api/v1/status` |
| `GET /api/preset?i=N` | `POST /api/v1/commands/preset` |
| `GET /api/tune?freq=N` | `POST /api/v1/commands/tune` |
| `GET /api/volume?v=N` | `POST /api/v1/commands/volume` |
| `GET /api/seek?dir=up/down` | `POST /api/v1/commands/seek` |
| `GET /api/preset/set?...` | `PUT /api/v1/presets/{index}` |
| `GET /api/oled?type=...` | `PUT /api/v1/display` |

Implementation should call the existing `applyPreset`, `applyFrequency`, `applyVolume`, `settingsTouch`, `oledBegin`, and `oledRender` pathways. Do not implement a second version of these effects in route handlers or in Android.

---

## 6. Android application architecture

### Technology baseline

- **Language:** Kotlin.
- **UI:** Jetpack Compose.
- **App pattern:** MVVM / unidirectional UI state.
- **Network:** Android `ConnectivityManager` + `WifiNetworkSpecifier` for the local-only radio AP; `HttpURLConnection` bound to the acquired `Network` for a minimal MVP, or OkHttp configured so its sockets use the acquired network.
- **Persistence:** DataStore for non-secret preferences. Store the AP password only if needed for reconnect, preferably encrypted using Android Keystore-backed encryption.
- **QR setup:** A QR scanner/parser that validates schema version and device ID before accepting credentials.
- **Android baseline:** Recommend API 29+ for the first release because the primary automated local-AP flow uses `WifiNetworkSpecifier`. If API 26–28 support is later required, add an explicit manual Wi-Fi-settings fallback and verify the active Wi-Fi route before API requests; do not pretend the newer system approval flow exists there.

`WifiNetworkSpecifier` is the supported app-request mechanism for a user-approved local Wi-Fi network on Android 10/API 29 and later. Android owns the consent/connection UI, and the app should retain the returned `Network` object for requests to the radio. See the official [WifiNetworkSpecifier reference](https://developer.android.com/reference/android/net/wifi/WifiNetworkSpecifier). Do not build the design around silently toggling Wi-Fi on from the app; guide the user to the system Wi-Fi panel/settings when Wi-Fi is off. The official settings panel action is documented as `Settings.Panel.ACTION_WIFI` (API 29+): [Android Settings.Panel](https://developer.android.com/reference/android/provider/Settings.Panel).

### App modules

```text
app
├── presentation/
│   ├── ConnectScreen
│   ├── RadioScreen
│   ├── PresetEditorScreen
│   └── SettingsScreen
├── connection/
│   ├── RadioConnectionCoordinator
│   ├── WifiPermissionAndSettingsFlow
│   ├── WifiNetworkRequester
│   └── ConnectionStateMachine
├── data/
│   ├── RadioApiClient
│   ├── RadioRepository
│   ├── RadioDto / StatusDto / ErrorDto
│   └── DevicePairingStore
└── domain/
    ├── RadioState
    ├── RadioCommand
    └── ConnectionState
```

### Core Android state machine

```text
DISCONNECTED
   │ user taps Connect / app opens with a saved device
   ▼
CHECK_WIFI
   ├── Wi-Fi disabled ──▶ WIFI_DISABLED ──▶ open system Wi-Fi panel/settings
   │                                         │ return / re-check
   │                                         └──────────────▶ CHECK_WIFI
   ▼ Wi-Fi enabled
REQUESTING_NETWORK
   ├── user rejects or timeout ──▶ RECOVERY / show instructions
   ▼ onAvailable(Network)
VERIFYING_DEVICE
   ├── health timeout / wrong device ID ──▶ RECOVERY
   ▼ health + info pass
CONNECTED
   ├── API operation failed ──▶ retry read once, then status refresh
   ├── network lost ──────────▶ RECONNECTING
   └── user disconnects ──────▶ DISCONNECTED
RECONNECTING
   ├── request renewed and identity passes ──▶ CONNECTED
   └── bounded attempts exhausted ───────────▶ RECOVERY
```

Each state has a user-readable message. Avoid an infinite spinner. Suggested time limits: 20–30 seconds for obtaining a Wi-Fi network, 3 seconds per health/info request, one short retry for a read, and a capped reconnect sequence with a visible “Try again” action. Tune these values during tests on real phones.

### Connection rules

1. **Wi-Fi off:** Explain that Wi-Fi is required and open `Settings.Panel.ACTION_WIFI`; fall back to `Settings.ACTION_WIFI_SETTINGS` if the panel cannot be opened. On return, re-check instead of assuming it was enabled.
2. **Ask Android to connect:** On Android 10/API 29+, use a `WifiNetworkSpecifier` for the SSID and WPA2 passphrase decoded from the device QR / saved pairing record. Expect a user approval step; do not claim the app can silently force the connection. If older Android versions are supported later, use a separate manual-connect flow rather than emulating the newer API.
3. **Keep the returned Network:** A local SoftAP often has no internet, so Android may keep mobile data or another internet network as the default. Send radio HTTP requests through the `Network` delivered in `onAvailable`, e.g. `network.openConnection(URL("http://192.168.4.1/api/v1/health"))`, or use a network-bound HTTP client. Do not assume ordinary default-network requests will reach the radio.
4. **Verify the device:** A successful Wi-Fi connection is not enough. Call `/api/v1/health`, then `/api/v1/info`, and compare the reported device ID to the selected/paired device.
5. **Do not require internet:** Mark the radio connection healthy based on successful local API responses, not public internet checks.
6. **Disconnect cleanly:** Unregister the network callback when the user disconnects or the connection coordinator is permanently disposed. Avoid leaking callbacks during Activity recreation.
7. **Use bounded recovery:** When the AP disappears or the phone roams away, stop control polling, show “Radio disconnected,” release the stale network reference, and provide a reconnect action. Resume polling only after identity verification succeeds again.

### Screens

**A. Connect / device selection** — Scan QR, show decoded SSID and device ID, Connect button, Wi-Fi-off helper, and fallback instructions for manual Wi-Fi setup plus opening `http://192.168.4.1/`.

**B. Connecting** — A clear step indicator: Wi-Fi status → Android network approval → connecting to device → verifying radio. Show cancellation and timeout recovery.

**C. Radio control** — Current frequency, preset name, volume, seek up/down, tune input, signal/stereo status, and radio-present warning. Keep controls disabled while a conflicting command is running, but keep read-only status visible.

**D. Preset editor** — Rename/tune a slot with client-side validation and a firmware-returned confirmation.

**E. Device / diagnostics** — Device ID, board, firmware build, API version, AP SSID/IP, last connection error, reconnect, forget device, and instructions to use the web UI.

### UI polling and command policy

- Poll `/api/v1/status` about once per second only while the screen is visible and connected, or slow to 2–3 seconds if the API is under load.
- Coalesce rapid slider changes; send the final volume value after a short debounce rather than one request per pixel of movement.
- Render values returned by the ESP, not just the intended slider/command values.
- During seek, show a busy state and allow other reads to pause. Once seek completes, refresh the full status.
- If a mutation response times out, read status before resending the mutation.
- Keep the connection alive for the current radio screen; suspend polling in background, and re-verify when returning.

---

## 7. Pairing and local onboarding

### Recommended customer path

1. User powers on the radio.
2. The label / quick-start card gives the radio's QR code, SSID, and password.
3. User opens the app and scans the QR code.
4. App asks to enable Wi-Fi if needed.
5. App asks Android to connect to the radio SSID; Android may display its own confirmation.
6. App requests the AP network and verifies `/api/v1/info` and device ID.
7. App opens the radio controls.

### QR payload format

Use a versioned, validated payload. Example for architecture only:

```json
{
  "schema": 1,
  "device_type": "fm-radio",
  "device_id": "A1B2C3",
  "ssid": "FM-Radio-A1B2C3",
  "password": "per-unit-random-passphrase",
  "base_url": "http://192.168.4.1"
}
```

Never use the example password in production. Reject malformed QR data, unexpected device types, unsupported schema versions, and URLs outside the expected local address policy. The app should not fetch arbitrary URLs from a QR code.

### Browser fallback

Print `http://192.168.4.1/` in the quick-start card. It remains the universal recovery path when the app is unavailable. Captive-portal auto-opening can be added later, but must not be the only onboarding path: Android/iOS popup behaviour varies, and users should always have the fixed IP and app-based path.

---

## 8. Reliability, failsafes and error handling

### Firmware principles

- Radio starts before networking; networking must not block basic listening.
- If the AP fails to start, print a clear serial error and retry with a bounded backoff. Do not loop-reset continuously.
- If the radio IC is absent, keep the HTTP service alive and report `radio.present=false`.
- If OLED is absent or its controller setting is wrong, radio/API operations continue.
- Invalid API requests must return a controlled error and must not corrupt settings.
- Continue debounced flash writes; do not persist after every poll.
- Cap strings and numeric fields before storing or serializing.
- Keep OTA authentication active and disable unsafe public/shared credentials in retail firmware.
- Keep the AP's identity stable across reboot; do not randomly change SSID/password on every boot.

### Android recovery matrix

| Failure | Detection | User-facing response | Recovery |
|---|---|---|---|
| Wi-Fi disabled | System Wi-Fi state is off | “Turn on Wi-Fi to connect to your radio” | Open Wi-Fi panel/settings; check again on return |
| User rejects system Wi-Fi dialog | `onUnavailable` / callback failure | “Connection was cancelled” | Button to retry or show manual steps |
| Wrong AP/password | Network request fails or timeout | “Could not join this radio network” | Re-scan QR, check power/SSID, retry |
| Wi-Fi connected but no HTTP route | Health request timeout | “Connected to Wi-Fi, but radio did not respond” | Try fixed IP, verify correct AP, renew network request |
| Wrong device responded | `/api/v1/info` ID mismatch | “This is not the selected radio” | Stop commands and return to device selection |
| Radio IC absent | `radio.present=false` | Keep UI open; show hardware warning | Allow diagnostics; do not crash/disconnect app |
| Seek takes longer | Command still busy / timeout not elapsed | “Searching for a station…” | Wait for completion; refresh status after result |
| AP disappeared/reboot | Network callback lost or repeated HTTP failure | “Radio disconnected” | Renew network request; verify identity again |
| Firmware update in progress | OTA state or API health unavailable | “Radio is updating; reconnect after reboot” | Wait, reconnect, then verify build identity |
| App backgrounded | Lifecycle event | Pause polling | Recheck connectivity and health on foreground |

### Seek design

The current seek path may block the HTTP event loop while the radio reports seek completion. For an MVP, use a sufficiently long request timeout and a visible busy state. For the robust release, convert seek into an asynchronous operation:

1. `POST /api/v1/commands/seek` returns `202 Accepted` and a job ID.
2. Firmware starts seek and continues servicing the web server, with a `seekInProgress` guard to prevent conflicting tune/seek commands.
3. App polls `GET /api/v1/jobs/{id}` or reads status until the job is `completed` or `failed`.
4. Firmware returns the final frequency and refreshed status.
5. A bounded timeout clears the busy state and returns a stable error if the tuner never asserts completion.

This is a second-phase improvement; it should not delay the first AP-mode prototype. The full-band scan (section 4, "Station scan and station log") uses the same STC/SF polling. Build the async single seek first, then make scan a loop over it.

---

## 9. Security model

The radio is a local appliance, not an internet service, but a nearby person who joins the AP can control it and may reach the OTA page. Apply these rules:

1. Unique per-unit WPA2 credentials. No universal production password; SSID/MAC-derived values are not secrets.
2. OTA requires authentication, and production OTA credentials must be unique or managed safely. Avoid exposing the update flow in the consumer UI.
3. Validate every command and preset index on the ESP, even if Android validates first.
4. Do not expose credentials through `/api/status`, logs, firmware identity responses, or status pages.
5. Use a dedicated device identity field so an app cannot accidentally control a different radio that happens to use the same fixed IP.
6. Do not rely on a secret API path as authentication. Wi-Fi membership is the first access boundary; OTA requires its own protection.
7. Avoid destructive operations without an explicit confirmation/physical recovery path.
8. On the owner's LAN, anyone on that network can reach the radio. This is why the OTA password must be set by the owner during setup (`NetRecord.otaPass`) rather than shared in public firmware. The setup AP itself is open or uses the label passphrase only while in `SETUP_AP`/fallback mode.
9. Router and hotspot passwords never leave the device: no API, log, or status page returns them.
10. Document that traffic is plain HTTP on the local link in the first version. Because that link is limited to the radio AP, this may be acceptable for the local MVP; for stronger threat models, evaluate HTTPS/certificate provisioning and authenticated API sessions.

---

## 10. Repository and deliverable layout

Recommended repository structure once the network logic stabilizes:

```text
fm-radio/
├── firmware/
│   ├── platformio.ini                 # optional; keep Arduino CLI builds if preferred
│   ├── src/
│   │   ├── main.ino                   # setup/loop and wiring together
│   │   ├── BoardProfile.h
│   │   ├── RDA5807M.h/.cpp
│   │   ├── RadioController.h/.cpp
│   │   ├── SettingsStore.h/.cpp
│   │   ├── NetworkManager.h/.cpp
│   │   ├── ApiRouter.h/.cpp
│   │   ├── DisplayService.h/.cpp
│   │   └── OtaService.h/.cpp
│   └── secrets.h.example              # placeholders only; no real secrets
├── android/
│   ├── app/src/main/java/.../connection/
│   ├── app/src/main/java/.../data/
│   ├── app/src/main/java/.../domain/
│   └── app/src/main/java/.../presentation/
├── docs/
│   ├── ARCHITECTURE.md
│   ├── API-v1.md
│   ├── provisioning.md
│   └── test-matrix.md
└── tools/
    └── provision_unit.py              # optional production credential/label tool
```

Do not force a file split as part of the first networking change. First demonstrate correct AP operation in the existing sketch; split modules after that behaviour is covered by tests.

---

## 11. Implementation phases and exit criteria

### Phase 0 — Baseline and regression snapshot

- Build the existing sketch for ESP8266 and ESP32-C3.
- Record current browser functionality, preset persistence, button operation, OLED modes, and OTA behaviour.
- Save known-good firmware binaries and note flash-layout/build settings.

**Exit:** Existing baseline compiles for both targets and the regression checklist is recorded.

### Phase 1 — AP-only network

- Replace station-first `wifiBegin()` with AP initialization.
- Add unique SSID derivation and a development credential.
- Set `192.168.4.1/24` and start the HTTP server in AP mode.
- Split `wifiUp()` into AP and STA predicates, and change status JSON/OLED footer to AP state/IP.
- Disable router-retry logic for AP-only firmware.
- Do not yet add captive DNS or Android app dependencies.

**Exit:** Phone/laptop joins the AP and opens `http://192.168.4.1/`; the radio, button, presets, volume, OLED, and settings persistence still work.

### Phase 1b — Wi-Fi provisioning (AP + STA)

- Add `NetRecord` (EEPROM offset 256 / `Preferences` `"net"`) with up to 3 networks and the owner's OTA password.
- Add the captive `/setup` page (DNSServer + `WiFi.scanNetworks()`), test-join before saving.
- Implement the `SETUP_AP → STA_CONNECTING → STA / STA_AP_FALLBACK` state machine. Start mDNS, DNS-SD, and OTA in every mode.
- Make the hostname unique (`radio-XXXXXX`). Add the hold-at-boot Wi-Fi reset.
- Remove `WIFI_SSID`/`WIFI_PASSWORD` from the retail build.

**Exit:** A unit built without `secrets.h` Wi-Fi values is set up from a phone and joins (a) a home router and (b) an Android and an iPhone hotspot. After the router is switched off it falls back to the AP within about 30 s and rejoins when the router returns. Presets survive all of this.

### Phase 2 — API health and device identity

- Add `/api/v1/health` and `/api/v1/info`.
- Add API version and stable device ID.
- Add standard JSON error responses and `Cache-Control: no-store`.
- Add contract examples to `docs/API-v1.md`.

**Exit:** `curl` or a browser can distinguish “AP is up”, “HTTP server is up”, “this is an FM radio”, and “the radio IC is/is not present”.

### Phase 3 — Versioned command API

- Add `/api/v1/status` and command endpoints.
- Route commands through the existing application functions.
- Validate methods, argument ranges, indexes, and preset names.
- Update the browser UI to use the new API, while temporarily retaining legacy aliases.

**Exit:** Every browser control works exclusively through the new API; there is one authoritative radio state.

### Phase 4 — Android connectivity proof-of-concept

- Build a minimal Kotlin screen with QR/manual device entry.
- Handle Wi-Fi-off settings flow and Android network approval.
- Use the returned `Network` to call `/api/v1/health` and `/api/v1/info`.
- Show the verified device ID and current radio status.

**Exit:** On at least two Android versions/devices, the app connects to the AP despite no internet and requests the endpoint over the radio network rather than mobile data.

### Phase 5 — Radio UI

- Add status view, presets, tune, volume, seek and OLED settings.
- Bind UI to repository state, not direct HTTP calls from composables.
- Add debounced slider requests and command progress/error messages.

**Exit:** All app controls work and the web UI continues to work concurrently.

### Phase 6 — Failure recovery and seek hardening

- Add connection state machine and bounded retries.
- Add “connected to Wi-Fi but device not reachable” handling.
- Verify device identity after every reconnect.
- Implement asynchronous seek job if required by latency testing.
- Test reboot and OTA interruption/recovery.

**Exit:** No infinite spinners; every major failure has a visible action and returns to a known state.

### Phase 6b — Station scan and station log

- "Save current station to preset" in the app and the web UI (existing `/api/preset/set`).
- Non-blocking `scanTick()` with SF band-edge termination and a `BUSY` guard.
- LittleFS `/stations.bin` with 24 B `StationRecord`, the duplicate rule, and a single write per scan.
- Paged `/api/v1/stations`; app sends phone location with `POST /api/v1/scan`.
- First-scan preset fill when presets are untouched factory values.

**Exit:** A full scan never stalls `/api/v1/status` for more than one poll interval. Scanning twice at the same location adds no duplicates. After a reboot or sketch OTA the log is intact. Scans at two places 50 km apart produce separate entries for the same frequency.

### Phase 7 — Production provisioning and security

- Add a per-unit credential/provisioning workflow.
- Generate QR/label output and factory-reset procedure.
- Remove development defaults from the retail build.
- Lock down OTA and review debug logs/response payloads for secrets.

**Exit:** Two physical units have different credentials and IDs; the app can connect to each by scanning its own QR and cannot confuse the units.

### Phase 8 — Optional enhancements

Only after the core workflow is stable:

- Captive portal DNS to improve browser onboarding, with a fixed-IP fallback.
- Improv Wi-Fi over serial with an ESP Web Tools flasher page for released binaries.
- BLE provisioning on ESP32-C3 only.
- RDS PS names during scan, if not done in Phase 6b.
- mDNS/`radio.local` convenience on networks where name resolution works.
- In-app authenticated OTA with signed firmware only after a complete update-security plan.

---

## 12. Test matrix

| Test | ESP target | Expected result |
|---|---|---|
| Clean boot, radio present, no phone | Both | Audio begins; AP starts; button works |
| Clean boot, RDA5807M missing | Both | AP/HTTP remain available; UI reports radio absent |
| No OLED attached | Both | No boot failure; web and radio work |
| SSD1306 / SH1106 selection | Both supported builds | Correct display mode persists across reboot |
| Change frequency/volume/presets | Both | Correct hardware state; settings save after debounce |
| Reboot after settings changes | Both | Saved radio settings restored; CRC invalidity falls back to defaults |
| AP address | Both | Phone receives local address and can open `192.168.4.1` |
| Router/internet absent | Both | Radio and button unaffected; AP fallback within ~30 s; STA retries no more than every 60 s |
| First boot, no saved network | Both | Setup AP + captive page; credentials tested before saving |
| Wrong router password entered | Both | Setup page reports it; nothing saved; AP stays up |
| Phone hotspot (Android 2.4 GHz, iPhone "Maximize Compatibility") | Both | Joins; reachable by IP and app DNS-SD |
| WPA3-only network | ESP8266 | Clear "not supported" message, not an endless retry |
| Two radios on one LAN | Both | Distinct `radio-XXXXXX` hostnames and DNS-SD entries |
| Hold button 10 s at boot | Both | Saved networks cleared; presets and station log kept |
| Full-band scan while app polls | Both + app | Status keeps answering; tune/seek return `BUSY` until done |
| Rescan at same place | Both | Log count unchanged; RSSI/lastSeen updated |
| Power cut during scan commit | Both | Log readable on boot; at most the last record lost |
| Sketch OTA after scans | Both | Station log and presets preserved |
| Two radio units powered | Both | Distinct SSIDs/IDs; QR selects the intended device |
| Wi-Fi disabled on phone | Android app | App explains requirement and opens Wi-Fi settings/panel |
| User denies connection | Android app | App shows cancellation; no infinite spinner |
| AP joined, internet absent | Android app | API calls still use the acquired radio `Network` and succeed |
| Wrong password / wrong AP | Android app | Clear recoverable error; no false connected state |
| AP drops/reboots while connected | Android app | UI disconnects, stops polling, can reconnect and re-verify identity |
| Write request times out | Android app | App reads status before repeating command |
| Seek on weak signal | Both + app | Seek stays bounded; HTTP/app does not falsely report disconnection |
| OTA authentication failure | Both | Update rejected; normal radio remains usable |
| OTA upload interruption | Both | Failure is visible and recovery path is documented/tested |
| Legacy route regression | Both | Existing embedded webpage works through compatibility routes until migration is complete |

### Build checks

Keep the existing dual-target compile checks as CI/manual gates:

```bash
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 <sketchdir>
arduino-cli compile --fqbn esp32:esp32:esp32c3 <sketchdir>
```

The exact board FQBN should match the actual ESP-12E board profile and flash settings used by the project.

---

## 13. Definition of done for release 1

Release 1 is ready when all the following are true:

- The ESP starts its own secured AP and remains usable without a router or internet.
- A unit with no build-time Wi-Fi credentials can be set up by its owner to join their router or phone hotspot, and falls back to its AP when that network is unavailable.
- Found stations can be saved to presets, and a full-band scan is stored in the LittleFS station log.
- `http://192.168.4.1/` opens the existing radio UI.
- The Android app can join through Android's supported user-approved network flow, including the Wi-Fi-disabled and user-cancelled cases.
- App HTTP requests are sent on the acquired radio network and are verified against `/api/v1/info`.
- Frequency, presets, volume, seek, and OLED settings use the shared firmware API.
- Physical button, radio playback, settings persistence, and optional display still function without the app.
- Radio absence, connection loss, bad input, timeout, and reboot all have bounded failure/recovery behaviour.
- Production credentials are unique per device; no real secrets are committed to the repository.
- ESP8266 and ESP32-C3 builds both pass the project compile and regression checklist.

## 14. Known gotchas and open risks

Review of this proposal against `Src/test_I2C_Radio/test_I2C_Radio.ino` (2026-10-25). Line numbers refer to that sketch at the time of review.

### 14.0 Fix immediately

- **The real OTA password is in a public comment.** The arduino-cli example in the section 8 header comment (around line 913, `--upload-field password=...`) contains the value actually used in the local `secrets.h`. Being gitignored does not help when the value is also in the sketch. Rotate the password and replace the comment with a placeholder.

### 14.1 Firmware

1. **IDE OTA and mDNS only start on station association.** `otaBegin()` and `MDNS.begin()` live inside `networkTick()` on the `wifiUp()` false→true edge (lines ~1257–1265). In AP-only mode `wifiUp()` never becomes true, so the arduino-cli network port and `radio.local` silently disappear. Browser `/update` keeps working because its routes are registered in `setup()`. Move OTA/mDNS start into the AP bring-up path when replacing `networkTick()`.
2. **The proposed status JSON does not fit `jsonBuf[1024]`.** One preset in the v1 shape (`{"index":9,"frequency_10khz":10620,"name":"<12 chars>"}`) is ~60 bytes; ten presets plus the nested header exceed 1 KB. The current builder truncates silently, and once full the closing `]}` cannot be written, so clients receive invalid JSON with HTTP 200. Size the buffer (watch ESP8266 heap) or shorten keys, and make overflow an error.
3. **No JSON request parsing exists.** POST/PUT bodies arrive as `server.arg("plain")`, which on ESP8266 requires a non-form `Content-Type`. Path parameters such as `/api/v1/presets/{index}` need `uri/UriBraces.h`. ArduinoJson (or a small hand parser) is a new dependency that must compile on both cores.
4. **Legacy routes turn bad input into real actions.** `String::toInt()` returns 0 for non-numeric text, so `?i=abc` selects preset 0 and `?freq=abc` clamps to 87.0 MHz and is persisted. Separately, the current contract *clamps* volume and frequency; "reject invalid input" is a behaviour change. Decide per field which one v1 does and document it.
5. **Seek blocks `loop()` for up to ~3 s and then tunes again.** `seek()` busy-waits on STC, then the route calls `applyFrequency(landed)`, which rewrites the channel and waits another 150 ms. STC/SF are never checked, so a timed-out seek reports the old frequency as success. An async seek needs its own STC polling in `loop()`; `readStatus()` is rate-limited and does not read STC/SF.
6. **The web server is single-client.** Phase 5's exit criterion has the browser UI and the app both polling at 1 Hz while seek blocks for 3 s. Keep-alive connections from the Android HTTP client can monopolise `ESP8266WebServer` and starve the other client. Test concurrent clients early, not in Phase 6.
7. **SoftAP setup fails silently.** `WiFi.softAP()` returns false for a passphrase shorter than 8 or longer than 63 characters; the return value must be checked. On ESP32 Arduino core 3.x `WiFi.macAddress()` returns zeros until `WiFi.mode()` has run, so derive the SSID suffix after setting the mode (or from chip ID / eFuse MAC).
8. **Flash layout has no room for a second record yet.** ESP8266 calls `EEPROM.begin(sizeof(Settings)+8)`. A `ProvisioningRecord` needs a fixed offset beyond `Settings`, and every `EEPROM.begin()` must use the same total size. Also, a `SETTINGS_VER` mismatch currently resets to factory defaults (no migration code), so a version bump wipes user presets unless migration is written first.
9. **Provisioning is chicken-and-egg.** The label must be printed before shipping, so the per-unit passphrase cannot be generated on first boot. A serial/USB provisioning step (`tools/provision_unit.py`) must write the record and read it back for the label. "Erase all flash" in the IDE destroys it.
10. **`secrets.h` changes meaning.** `WIFI_SSID` / `WIFI_PASSWORD` currently mean "router to join". Reusing them for the AP is confusing; rename (e.g. `AP_DEV_PASSPHRASE`) and update `secrets.h.example`.
11. **Empty `OTA_PASSWORD` disables OTA authentication** (`otaAuthOk()` returns true). Retail builds should fail to compile if it is empty.
12. **`/api/oled` bypasses the "single control path".** It writes `cfg.oledType` directly rather than through an `apply*` helper. Acceptable, but say so in the API contract so a second path is not added elsewhere.

### 14.2 Android

13. **Cleartext HTTP is blocked by default on Android 9+.** Requests to `http://192.168.4.1` (or a LAN IP) fail until a `network_security_config` permits cleartext for those hosts.
14. **Permissions and Location state are missing from the state machine.** Depending on API level, `WifiNetworkSpecifier` needs `CHANGE_WIFI_STATE` plus location permission or `NEARBY_WIFI_DEVICES`, and on many devices system Location must be on or the request simply times out. Add an explicit permission/location state.
15. **A specifier network is app-scoped.** Chrome on the same phone cannot use it, and Android tears the network down when the request callback is released. Therefore: pausing polling in the background must not unregister the request (or the user sees the approval dialog again); "web UI works concurrently" (Phase 5) only holds from a second device or a manual Wi-Fi join; and the phone leaves its home Wi-Fi while connected.
16. **The browser fallback is weaker than stated.** After a manual join to an AP without internet, many phones prompt "stay connected?" or keep mobile data as the default route, so `http://192.168.4.1/` may not load until the user accepts or disables mobile data. Put this on the quick-start card.
17. **Use the `requestNetwork(request, callback, timeoutMs)` overload.** Without a timeout, `onUnavailable` may never fire, producing the infinite spinner the design wants to avoid.

### 14.3 Hardware and workflow

18. **AP mode may add RF noise to FM audio.** A SoftAP beacons every ~100 ms indefinitely and draws continuous current, which is harsher on a shared supply/ground with the RDA5807M than an idle station. Listen for buzz/hum during Phase 1 and check regulator headroom, especially on the ESP-12E.
19. **Developer reflash workflow regresses.** Without a router, network OTA requires the laptop to join the radio AP and lose internet while doing so. (AP + STA mostly fixes this: in STA mode OTA works over the LAN again.)

### 14.4 Shipping to other users (added 2026-10-29)

20. **`FACTORY_PRESETS` are one city's stations.** Elsewhere most slots are silent. The first-scan preset fill (section 4) fixes this without a `Settings` change.
21. **De-emphasis is regional.** `FM_DEEMPHASIS_50US = 1` is right for India, Europe, and most of Asia. The Americas and Korea need 75 µs. Make it a runtime setting if units leave the region.
22. **One `MDNS_HOSTNAME "radio"` for every unit** collides on a shared LAN. Derive a per-unit hostname.
23. **A shared OTA password in public firmware** lets anyone on a buyer's network flash their radio. Make the owner set it at setup, and refuse OTA until it is set.
24. **`jsonBuf[1024]` cannot hold a station list.** Paged, chunked responses are mandatory for `/api/v1/stations` (see also 14.1 item 2).
25. **Adding LittleFS costs flash and RAM.** The current ESP8266 build is 340,684 B IROM (32%) with 31,880 B of globals (39%). LittleFS adds roughly 20–30 KB of code (estimate) and small per-file buffers. Recheck the numbers after Phase 6b, and keep `eesz=4M2M` (OTA room ~1019 KB) rather than 4M3M (~512 KB).
26. **The log is erased by uploading a LittleFS image or by "Erase all flash".** Say so in the README, and never ship a data image for the radio.

---

## 15. External API references

- ESP8266 Arduino SoftAP and `softAPConfig`: https://github.com/esp8266/Arduino/blob/master/doc/esp8266wifi/soft-access-point-class.rst
- Android `WifiNetworkSpecifier`: https://developer.android.com/reference/android/net/wifi/WifiNetworkSpecifier
- Android Wi-Fi settings panel: https://developer.android.com/reference/android/provider/Settings.Panel
- ESP8266 `ESP8266WiFiMulti` and station class: https://github.com/esp8266/Arduino/blob/master/doc/esp8266wifi/station-class.rst
- ESP8266 filesystem / LittleFS and flash layouts: https://arduino-esp8266.readthedocs.io/en/latest/filesystem.html
- WiFiManager (alternative captive portal, ESP8266 + ESP32): https://github.com/tzapu/WiFiManager
- Improv Wi-Fi (serial/BLE provisioning) and ESP Web Tools: https://www.improv-wifi.com/ , https://esphome.github.io/esp-web-tools/

These references describe platform APIs. The exact firmware API schema and the app flow above are this project's proposed architecture, not platform guarantees.
