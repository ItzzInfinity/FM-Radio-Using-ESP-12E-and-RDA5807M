# FM Radio — Android feasibility app

Throwaway spike that controls the **current** station-mode firmware
(`Src/test_I2C_Radio/test_I2C_Radio.ino`) over the home Wi-Fi. It uses the
legacy `GET /api/*` routes exactly as the embedded web page does; nothing from
the `/api/v1` proposal is assumed.

## Build and install

```bash
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Kotlin + Jetpack Compose, minSdk 26, no third-party dependencies (org.json and
`HttpURLConnection` only).

## Use

1. Flash the radio, let it join the router, note the IP on the OLED footer.
2. Put the phone on the same Wi-Fi, open the app.
3. Tap **Scan network** (DNS-SD `_http._tcp`, finds the firmware's `radio`
   service) or type the IP, then **Connect**.

## What it is meant to prove

| Question | Where to look |
|---|---|
| Can an app drive every control the web page has? | Radio screen: presets, edit, tune, seek, volume, OLED |
| Does the single-client ESP server keep up with 1 Hz polling plus the web page open at the same time? | Diagnostics: round trip, ok/failed counts |
| Does seek (blocks the ESP for ~3 s) get mistaken for a disconnect? | Seek shows a busy state; polls skip instead of queueing |
| Does a write timeout lead to a re-read rather than a resend? | Message line after a slow command |
| Do network-bound sockets (`Network.openConnection`) work? This is what the AP-mode app needs | Diagnostics: *Bind requests to Wi-Fi* switch |
| mDNS from an app (`radio.local` cannot be resolved by name on Android) | Scan network |

Not covered, because the current firmware cannot do it: SoftAP join via
`WifiNetworkSpecifier`, QR pairing and device-ID checks. See §14 of
`../FM_Radio_refactor_Architecture_proposal.md`.

Cleartext HTTP is allowed to *any* host (`res/xml/network_security_config.xml`)
because the radio gets a DHCP address. Narrow it to `192.168.4.1` for the AP build.
