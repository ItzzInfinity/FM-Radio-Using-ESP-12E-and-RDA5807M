/*
 * ===========================================================================
 *  FM Radio  --  RDA5807M  --  ESP8266 (ESP-12E) / ESP32-C3
 * ===========================================================================
 *
 *  One sketch, two targets. Pick the board in the IDE (or with --fqbn) and
 *  flash -- the pin map and the platform APIs follow automatically, there is
 *  no second switch to keep in sync.
 *
 *  Features
 *    - Self-contained RDA5807M driver (no external radio library needed)
 *    - Web UI on http://radio.local : presets, volume, seek, manual tune,
 *      and editable preset slots
 *    - Station + volume + presets persist across power cycles
 *    - Optional SSD1306 / SH1106 OLED, auto-detected on the I2C bus and
 *      switchable from the web page without reflashing
 *    - Hardware button still cycles presets with no Wi-Fi at all
 *
 *  Wiring (unchanged from the original board)
 *    RDA5807M  SDA/SCL -> see PIN_SDA / PIN_SCL below, I2C addr 0x10 / 0x11
 *    OLED      SDA/SCL -> same bus, I2C addr 0x3C (or 0x3D)
 *    Button    -> PIN_BUTTON to GND (internal pull-up)
 *
 *  Build check:
 *    arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 <sketchdir>
 *    arduino-cli compile --fqbn esp32:esp32:esp32c3       <sketchdir>
 */

#include <Arduino.h>
#include <Wire.h>

// ===========================================================================
//  1. USER CONFIGURATION
// ===========================================================================

// --- Wi-Fi and OTA credentials ---------------------------------------------
// This repository is public on GitHub, so the credentials live in a separate
// gitignored file rather than in the sketch. Before the first build:
//
//   cp secrets.h.example secrets.h     # then edit secrets.h
//
// It defines WIFI_SSID, WIFI_PASSWORD, OTA_USERNAME and OTA_PASSWORD.
#include "secrets.h"

// Reachable as http://radio.local (and as the plain DHCP IP either way).
#define MDNS_HOSTNAME "radio"

// --- OTA (over-the-air firmware update) ------------------------------------
// The board lives in a sealed enclosure, so there are two independent ways to
// reflash it without opening the box. See section 8 for the details.
#define OTA_ENABLED 1

// OTA_PASSWORD guards BOTH update paths and OTA_USERNAME names the browser
// upload page's login; both come from secrets.h above.

// Shown on the web page so you can confirm an update actually landed.
#define FW_BUILD __DATE__ " " __TIME__

// --- Display ---------------------------------------------------------------
// OLED_ENABLED 0 compiles the display code out entirely: no Adafruit library
// is pulled in and the sketch is a pure headless radio.
#define OLED_ENABLED 1

// Default controller for a freshly-flashed board. The two chips are
// indistinguishable over I2C, so this is the starting guess -- if the module
// you fit shows garbage or is shifted a couple of pixels, switch it on the
// web page instead of reflashing. The choice is remembered in flash.
//   OLED_TYPE_SSD1306 | OLED_TYPE_SH1106 | OLED_TYPE_NONE
#define OLED_DEFAULT OLED_TYPE_SSD1306

// --- Radio -----------------------------------------------------------------
// 50 us de-emphasis is correct for India / Europe / most of the world.
// Set to 0 for the 75 us standard used in the Americas / South Korea.
#define FM_DEEMPHASIS_50US 1

// Soft mute pulls the audio down on very weak signals. Nice for seek, but it
// can make a marginal station breathe -- set to 0 if you prefer raw hiss.
#define FM_SOFTMUTE 1

// --- Presets ---------------------------------------------------------------
// Frequencies in 10 kHz units: 10400 == 104.00 MHz.
// These are the factory defaults; they can be renamed and re-tuned from the
// web page afterwards, and the edits are stored in flash.
#define PRESET_COUNT 10
static const uint16_t FACTORY_PRESETS[PRESET_COUNT] = {
  10620, 9270, 9350, 9430, 9830, 10130, 10260, 10400, 10480, 10640
};

// Volume 0..15 on a fresh board.
#define DEFAULT_VOLUME 13

// ===========================================================================
//  2. BOARD PIN MAP  +  PLATFORM SHIMS
// ===========================================================================

#if defined(ESP8266)
  #define BOARD_NAME "ESP8266 (ESP-12E)"
  constexpr int PIN_SDA    = 4;   // D2
  constexpr int PIN_SCL    = 5;   // D1
  // GPIO3 is UART0 RX. pinMode() below detaches it from the UART, so Serial
  // *output* on GPIO1 keeps working but Serial input does not. This matches
  // the original board, so the pin is left as-is.
  constexpr int PIN_BUTTON = 3;

  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
  #include <ESP8266mDNS.h>
  #include <EEPROM.h>
  #include <WiFiUdp.h>          // OTA needs WiFiUDP::stopAll() before flashing
  typedef ESP8266WebServer WebServerClass;
  // "Update" (UpdaterClass) is pulled in by the core's Arduino.h.

#elif defined(ARDUINO_ARCH_ESP32)
  #define BOARD_NAME "ESP32-C3"
  constexpr int PIN_SDA    = 8;
  constexpr int PIN_SCL    = 9;
  constexpr int PIN_BUTTON = 3;

  #include <WiFi.h>
  #include <WebServer.h>
  #include <ESPmDNS.h>
  #include <Preferences.h>
  #include <Update.h>
  typedef WebServer WebServerClass;

#else
  #error "Unsupported board: select an ESP8266 (ESP-12E / NodeMCU) or an ESP32-C3."
#endif

#if OTA_ENABLED
  #include <ArduinoOTA.h>       // same header name on both cores
#endif

// ===========================================================================
//  3. SETTINGS  +  PERSISTENCE
// ===========================================================================

enum : uint8_t {
  OLED_TYPE_NONE    = 0,
  OLED_TYPE_SSD1306 = 1,
  OLED_TYPE_SH1106  = 2
};

#define PRESET_NAME_LEN 13          // 12 visible characters + NUL
#define SETTINGS_MAGIC  0x524Du     // 'RM'
#define SETTINGS_VER    1u
#define PRESET_MANUAL   0xFFu       // presetIdx value meaning "manually tuned"

struct Preset {
  uint16_t freq10k;
  char     name[PRESET_NAME_LEN];
};

struct Settings {
  uint16_t magic;
  uint16_t version;
  uint16_t freq10k;                 // last tuned frequency, 10 kHz units
  uint8_t  presetIdx;               // 0..9, or PRESET_MANUAL
  uint8_t  volume;                  // 0..15
  uint8_t  oledType;                // OLED_TYPE_*
  uint8_t  pad;
  Preset   presets[PRESET_COUNT];
  uint16_t crc;
};

static Settings cfg;
static bool          cfgDirty       = false;
static unsigned long cfgDirtySince  = 0;
static const unsigned long CFG_SAVE_DELAY_MS = 2000;   // debounce flash writes

#if defined(ARDUINO_ARCH_ESP32)
static Preferences prefs;
#endif

// "104.0" from 10400. Buffer must hold at least 8 bytes.
static void freqToStr(uint16_t f10k, char *out, size_t len) {
  snprintf(out, len, "%u.%u", (unsigned)(f10k / 100), (unsigned)((f10k / 10) % 10));
}

static uint16_t crc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

static uint16_t settingsCrc(const Settings &s) {
  return crc16((const uint8_t *)&s, sizeof(Settings) - sizeof(uint16_t));
}

static void settingsDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic     = SETTINGS_MAGIC;
  cfg.version   = SETTINGS_VER;
  cfg.presetIdx = 0;
  cfg.freq10k   = FACTORY_PRESETS[0];
  cfg.volume    = DEFAULT_VOLUME;
  cfg.oledType  = OLED_DEFAULT;
  for (uint8_t i = 0; i < PRESET_COUNT; i++) {
    cfg.presets[i].freq10k = FACTORY_PRESETS[i];
    freqToStr(FACTORY_PRESETS[i], cfg.presets[i].name, PRESET_NAME_LEN);
  }
}

static void settingsLoad() {
  Settings tmp;
  bool ok = false;

#if defined(ESP8266)
  EEPROM.begin(sizeof(Settings) + 8);
  EEPROM.get(0, tmp);
  EEPROM.end();
  ok = true;
#else
  prefs.begin("radio", false);
  ok = (prefs.getBytes("cfg", &tmp, sizeof(tmp)) == sizeof(tmp));
  prefs.end();
#endif

  if (ok && tmp.magic == SETTINGS_MAGIC && tmp.version == SETTINGS_VER &&
      tmp.crc == settingsCrc(tmp)) {
    cfg = tmp;
    // Guard against a good CRC over out-of-range values.
    if (cfg.volume > 15) cfg.volume = DEFAULT_VOLUME;
    if (cfg.presetIdx >= PRESET_COUNT && cfg.presetIdx != PRESET_MANUAL) cfg.presetIdx = 0;
    if (cfg.freq10k < 8700 || cfg.freq10k > 10800) cfg.freq10k = FACTORY_PRESETS[0];
    if (cfg.oledType > OLED_TYPE_SH1106) cfg.oledType = OLED_DEFAULT;
    for (uint8_t i = 0; i < PRESET_COUNT; i++) {
      cfg.presets[i].name[PRESET_NAME_LEN - 1] = '\0';
      if (cfg.presets[i].freq10k < 8700 || cfg.presets[i].freq10k > 10800)
        cfg.presets[i].freq10k = FACTORY_PRESETS[i];
    }
    Serial.println(F("[cfg] restored from flash"));
  } else {
    settingsDefaults();
    Serial.println(F("[cfg] no valid settings, using factory defaults"));
  }
}

static void settingsSaveNow() {
  cfg.magic   = SETTINGS_MAGIC;
  cfg.version = SETTINGS_VER;
  cfg.crc     = settingsCrc(cfg);

#if defined(ESP8266)
  EEPROM.begin(sizeof(Settings) + 8);
  EEPROM.put(0, cfg);
  EEPROM.commit();
  EEPROM.end();
#else
  prefs.begin("radio", false);
  prefs.putBytes("cfg", &cfg, sizeof(cfg));
  prefs.end();
#endif

  cfgDirty = false;
  Serial.println(F("[cfg] saved"));
}

// Mark settings changed; the actual write happens once the user stops
// fiddling, so dragging the volume slider does not hammer the flash.
static void settingsTouch() {
  cfgDirty      = true;
  cfgDirtySince = millis();
}

static void settingsTick() {
  if (cfgDirty && millis() - cfgDirtySince >= CFG_SAVE_DELAY_MS) settingsSaveNow();
}

// ===========================================================================
//  4. RDA5807M DRIVER
// ===========================================================================
//
//  The chip answers on two I2C addresses:
//    0x10  sequential access -- writes start at register 0x02, reads at 0x0A,
//          both auto-incrementing
//    0x11  indexed access    -- write the register number, then 16 bits of data
//
//  Registers are 16 bit, big-endian on the wire.

#define RDA_ADDR_SEQ  0x10
#define RDA_ADDR_IDX  0x11

#define RDA_REG_CTRL  0x02
#define RDA_REG_CHAN  0x03
#define RDA_REG_R4    0x04
#define RDA_REG_VOL   0x05
#define RDA_REG_STAT  0x0A   // read-only: RDSR STC SF RDSS BLKE ST READCHAN
#define RDA_REG_RSSI  0x0B   // read-only: RSSI FM_TRUE FM_READY ...

// Register 0x02 bits
#define RDA_DHIZ       0x8000
#define RDA_DMUTE      0x4000
#define RDA_MONO       0x2000
#define RDA_BASS       0x1000
#define RDA_SEEKUP     0x0200
#define RDA_SEEK       0x0100
#define RDA_SKMODE     0x0080
#define RDA_RDS_EN     0x0008
#define RDA_NEW_METHOD 0x0004
#define RDA_SOFT_RESET 0x0002
#define RDA_ENABLE     0x0001

// Register 0x03 bits
#define RDA_TUNE       0x0010

// Register 0x04 bits
#define RDA_DE_50US    0x0800
#define RDA_SOFTMUTE   0x0200

// Register 0x0A / 0x0B bits
#define RDA_STC        0x4000
#define RDA_SF         0x2000
#define RDA_ST         0x0400
#define RDA_FM_TRUE    0x0100

// Band 87..108 MHz, 100 kHz spacing -> channel n is 87.00 MHz + n * 100 kHz.
#define FM_BAND_LOW10K   8700
#define FM_BAND_HIGH10K  10800
#define FM_STEP10K       10

class RDA5807M {
public:
  bool begin() {
    Wire.beginTransmission(RDA_ADDR_IDX);
    _present = (Wire.endTransmission() == 0);
    if (!_present) return false;

    writeReg(RDA_REG_CTRL, RDA_SOFT_RESET);
    delay(50);

    _ctrl = RDA_DHIZ | RDA_DMUTE | RDA_BASS | RDA_NEW_METHOD | RDA_ENABLE;
    writeReg(RDA_REG_CTRL, _ctrl);
    delay(50);

    uint16_t r4 = 0;
#if FM_DEEMPHASIS_50US
    r4 |= RDA_DE_50US;
#endif
#if FM_SOFTMUTE
    r4 |= RDA_SOFTMUTE;
#endif
    writeReg(RDA_REG_R4, r4);

    // 0x8880: INT_MODE, seek threshold 8, LNA on LNAP -- the datasheet's
    // recommended defaults, with the volume in the low nibble.
    _vol = DEFAULT_VOLUME;
    writeReg(RDA_REG_VOL, 0x8880 | _vol);
    return true;
  }

  bool present() const { return _present; }

  void setVolume(uint8_t v) {
    if (v > 15) v = 15;
    _vol = v;
    if (_present) writeReg(RDA_REG_VOL, 0x8880 | _vol);
  }
  uint8_t volume() const { return _vol; }

  // f10k is in 10 kHz units (10400 == 104.00 MHz).
  void setFrequency(uint16_t f10k) {
    _freq = clampFreq(f10k);
    if (!_present) return;
    uint16_t chan = (uint16_t)((_freq - FM_BAND_LOW10K) / FM_STEP10K);
    writeReg(RDA_REG_CHAN, (uint16_t)(chan << 6) | RDA_TUNE);   // band 00, space 00
    waitForSTC(150);
  }
  uint16_t frequency() const { return _freq; }

  // Hardware seek. Returns where it actually landed.
  uint16_t seek(bool up) {
    if (!_present) return _freq;
    uint16_t c = _ctrl | RDA_SEEK | RDA_SKMODE;     // SKMODE: stop at band edge
    if (up) c |= RDA_SEEKUP; else c &= (uint16_t)~RDA_SEEKUP;
    writeReg(RDA_REG_CTRL, c);
    waitForSTC(3000);                                // a full sweep can be slow
    writeReg(RDA_REG_CTRL, _ctrl);                   // clear the seek request
    readStatus(true);
    return _freq;
  }

  // Refreshes the cached RSSI / stereo / tuned flags, at most every 500 ms
  // unless forced.
  void readStatus(bool force = false) {
    if (!_present) return;
    unsigned long now = millis();
    if (!force && now - _lastStatus < 500) return;
    _lastStatus = now;

    Wire.requestFrom((uint8_t)RDA_ADDR_SEQ, (uint8_t)4);
    if (Wire.available() < 4) return;
    uint16_t rA = (uint16_t)(Wire.read() << 8); rA |= (uint8_t)Wire.read();
    uint16_t rB = (uint16_t)(Wire.read() << 8); rB |= (uint8_t)Wire.read();

    _stereo = (rA & RDA_ST) != 0;
    _tuned  = (rB & RDA_FM_TRUE) != 0;
    _rssi   = (uint8_t)((rB >> 9) & 0x7F);

    uint16_t chan = rA & 0x03FF;
    uint16_t f    = (uint16_t)(FM_BAND_LOW10K + chan * FM_STEP10K);
    if (f >= FM_BAND_LOW10K && f <= FM_BAND_HIGH10K) _freq = f;
  }

  uint8_t rssi()   const { return _rssi; }    // 0..127
  bool    stereo() const { return _stereo; }
  bool    tuned()  const { return _tuned; }

  static uint16_t clampFreq(uint16_t f10k) {
    if (f10k < FM_BAND_LOW10K)  f10k = FM_BAND_LOW10K;
    if (f10k > FM_BAND_HIGH10K) f10k = FM_BAND_HIGH10K;
    // Snap to the 100 kHz channel grid.
    return (uint16_t)(FM_BAND_LOW10K +
                      ((f10k - FM_BAND_LOW10K + FM_STEP10K / 2) / FM_STEP10K) * FM_STEP10K);
  }

private:
  void writeReg(uint8_t reg, uint16_t val) {
    Wire.beginTransmission(RDA_ADDR_IDX);
    Wire.write(reg);
    Wire.write((uint8_t)(val >> 8));
    Wire.write((uint8_t)(val & 0xFF));
    Wire.endTransmission();
  }

  uint16_t readReg0A() {
    Wire.requestFrom((uint8_t)RDA_ADDR_SEQ, (uint8_t)2);
    if (Wire.available() < 2) return 0;
    uint16_t v = (uint16_t)(Wire.read() << 8);
    return v | (uint8_t)Wire.read();
  }

  // Poll the seek/tune-complete flag. Bounded so a dead chip cannot wedge the
  // sketch, and yields so Wi-Fi keeps being serviced.
  void waitForSTC(unsigned long timeoutMs) {
    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
      if (readReg0A() & RDA_STC) break;
      delay(5);
    }
  }

  bool          _present    = false;
  uint16_t      _ctrl       = 0;
  uint16_t      _freq       = FM_BAND_LOW10K;
  uint8_t       _vol        = DEFAULT_VOLUME;
  uint8_t       _rssi       = 0;
  bool          _stereo     = false;
  bool          _tuned      = false;
  unsigned long _lastStatus = 0;
};

static RDA5807M radio;

// ===========================================================================
//  5. OPTIONAL OLED
// ===========================================================================
//
//  Both drivers are declared but only the selected one gets begin() called,
//  so only one framebuffer is ever allocated. Everything except clearDisplay()
//  and display() goes through the shared Adafruit_GFX base, so the drawing
//  code below is written once.

#if OLED_ENABLED
  #include <Adafruit_GFX.h>
  #include <Adafruit_SSD1306.h>
  #include <Adafruit_SH110X.h>

  #define OLED_W 128
  #define OLED_H 64
  #define OLED_ON 1                 // SSD1306_WHITE and SH110X_WHITE are both 1

  static Adafruit_SSD1306 oledSSD(OLED_W, OLED_H, &Wire, -1);
  static Adafruit_SH1106G oledSH(OLED_W, OLED_H, &Wire, -1);

  static Adafruit_GFX *gfx        = nullptr;
  static bool          oledActive = false;
  static uint8_t       oledAddr   = 0;
#endif

// Probes both usual OLED addresses. 0 == nothing there.
static uint8_t oledProbe() {
  const uint8_t addrs[2] = { 0x3C, 0x3D };
  for (uint8_t i = 0; i < 2; i++) {
    Wire.beginTransmission(addrs[i]);
    if (Wire.endTransmission() == 0) return addrs[i];
  }
  return 0;
}

// Brings the display up for the given controller type. Safe to call again at
// runtime when the user switches controller from the web page.
static void oledBegin(uint8_t type) {
#if OLED_ENABLED
  oledActive = false;
  gfx        = nullptr;

  if (type == OLED_TYPE_NONE) {
    Serial.println(F("[oled] disabled"));
    return;
  }

  oledAddr = oledProbe();
  if (!oledAddr) {
    Serial.println(F("[oled] none found on the bus (that is fine, it is optional)"));
    return;
  }

  if (type == OLED_TYPE_SSD1306) {
    // reset=false (no reset pin), periphBegin=false (Wire is already up).
    if (oledSSD.begin(SSD1306_SWITCHCAPVCC, oledAddr, false, false)) {
      gfx = &oledSSD;
      oledActive = true;
    }
  } else {
    if (oledSH.begin(oledAddr, false)) {
      gfx = &oledSH;
      oledActive = true;
    }
  }

  if (oledActive) {
    gfx->setTextWrap(false);
    Serial.printf("[oled] %s at 0x%02X\n",
                  (type == OLED_TYPE_SSD1306) ? "SSD1306" : "SH1106", oledAddr);
  } else {
    Serial.println(F("[oled] found a device but init failed"));
  }
#else
  (void)type;
#endif
}

static inline void oledClear() {
#if OLED_ENABLED
  if (!oledActive) return;
  if (gfx == (Adafruit_GFX *)&oledSSD) oledSSD.clearDisplay(); else oledSH.clearDisplay();
#endif
}

static inline void oledShow() {
#if OLED_ENABLED
  if (!oledActive) return;
  if (gfx == (Adafruit_GFX *)&oledSSD) oledSSD.display(); else oledSH.display();
#endif
}

// Forward declarations used by the display code.
static const char *currentStationName();
static bool        wifiUp();

static void oledRender() {
#if OLED_ENABLED
  if (!oledActive) return;

  char fbuf[10];
  freqToStr(cfg.freq10k, fbuf, sizeof(fbuf));

  oledClear();
  gfx->setTextColor(OLED_ON);

  // Top line: station name, plus a stereo marker on the right.
  gfx->setTextSize(1);
  gfx->setCursor(0, 0);
  gfx->print(currentStationName());
  if (radio.stereo()) { gfx->setCursor(110, 0); gfx->print(F("ST")); }

  // Frequency, large.
  gfx->setTextSize(3);
  gfx->setCursor(0, 12);
  gfx->print(fbuf);
  gfx->setTextSize(1);
  gfx->setCursor(94, 29);
  gfx->print(F("MHz"));

  // Signal strength, five bars in the top right under the ST marker.
  uint8_t bars = (uint8_t)((radio.rssi() * 5) / 64);
  if (bars > 5) bars = 5;
  for (uint8_t i = 0; i < 5; i++) {
    int16_t h = (int16_t)(2 + i * 2);
    int16_t x = (int16_t)(96 + i * 3);
    if (i < bars) gfx->fillRect(x, 10 - h, 2, h, OLED_ON);
    else          gfx->drawRect(x, 10 - h, 2, h, OLED_ON);
  }

  // Volume bar.
  gfx->setCursor(0, 40);
  gfx->print(F("VOL"));
  gfx->drawRect(24, 40, 104, 7, OLED_ON);
  if (radio.volume() > 0)
    gfx->fillRect(26, 42, (int16_t)((100 * radio.volume()) / 15), 3, OLED_ON);

  // Bottom line: how to reach it.
  gfx->setCursor(0, 56);
  if (!radio.present())    gfx->print(F("RDA5807M not found"));
  else if (wifiUp())       gfx->print(WiFi.localIP().toString());
  else                     gfx->print(F("Wi-Fi: connecting..."));

  oledShow();
#endif
}

// ===========================================================================
//  6. RADIO STATE
// ===========================================================================

static const char *currentStationName() {
  if (cfg.presetIdx < PRESET_COUNT) return cfg.presets[cfg.presetIdx].name;
  return "Manual";
}

// Applies a frequency and works out whether it happens to match a preset.
static void applyFrequency(uint16_t f10k, uint8_t presetIdx) {
  cfg.freq10k = RDA5807M::clampFreq(f10k);
  radio.setFrequency(cfg.freq10k);

  if (presetIdx < PRESET_COUNT) {
    cfg.presetIdx = presetIdx;
  } else {
    cfg.presetIdx = PRESET_MANUAL;
    for (uint8_t i = 0; i < PRESET_COUNT; i++)
      if (cfg.presets[i].freq10k == cfg.freq10k) { cfg.presetIdx = i; break; }
  }

  char fbuf[10];
  freqToStr(cfg.freq10k, fbuf, sizeof(fbuf));
  Serial.printf("[radio] %s MHz  (%s)\n", fbuf, currentStationName());

  radio.readStatus(true);
  settingsTouch();
  oledRender();
}

static void applyPreset(uint8_t i) {
  if (i >= PRESET_COUNT) return;
  applyFrequency(cfg.presets[i].freq10k, i);
}

static void applyVolume(uint8_t v) {
  if (v > 15) v = 15;
  cfg.volume = v;
  radio.setVolume(v);
  settingsTouch();
  oledRender();
}

static void nextPreset() {
  uint8_t next = (cfg.presetIdx >= PRESET_COUNT) ? 0
                                                 : (uint8_t)((cfg.presetIdx + 1) % PRESET_COUNT);
  applyPreset(next);
}

// ===========================================================================
//  7. WEB UI
// ===========================================================================

static WebServerClass server(80);
static bool mdnsStarted = false;
static bool otaStarted  = false;

static bool wifiUp() { return WiFi.status() == WL_CONNECTED; }

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>FM Radio</title>
<style>
:root{--bg:#0d1117;--card:#161b22;--line:#272e38;--fg:#e6edf3;--dim:#8b949e;--acc:#f0883e;--ok:#3fb950}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.4 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;
 padding:16px;max-width:520px;margin-inline:auto;-webkit-text-size-adjust:100%}
h1{font-size:15px;font-weight:600;letter-spacing:.08em;text-transform:uppercase;color:var(--dim);margin:0 0 12px}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px;margin-bottom:12px}
.freq{font-size:52px;font-weight:700;line-height:1;font-variant-numeric:tabular-nums}
.freq span{font-size:18px;font-weight:500;color:var(--dim);margin-left:6px}
.name{color:var(--acc);font-weight:600;margin-top:6px;min-height:22px}
.meta{color:var(--dim);font-size:13px;margin-top:8px;display:flex;gap:12px;flex-wrap:wrap;align-items:center}
.dot{width:8px;height:8px;border-radius:50%;background:#444;display:inline-block;margin-right:5px}
.dot.on{background:var(--ok)}
.bar{height:6px;background:#21262d;border-radius:3px;overflow:hidden;flex:1;min-width:60px;max-width:110px}
.bar i{display:block;height:100%;background:var(--acc)}
button{font:inherit;color:var(--fg);background:#21262d;border:1px solid var(--line);border-radius:10px;
 padding:11px 14px;cursor:pointer;-webkit-tap-highlight-color:transparent}
button:active{transform:translateY(1px)}
button.acc{background:var(--acc);border-color:var(--acc);color:#111;font-weight:600}
.row{display:flex;gap:8px;align-items:center}
.row>*{flex:1}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(94px,1fr));gap:8px}
.grid button{padding:10px 6px;text-align:center;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.grid button.sel{border-color:var(--acc);color:var(--acc)}
.grid button b{display:block;font-size:11px;color:var(--dim);font-weight:500;margin-top:2px}
.grid button.sel b{color:var(--acc)}
input,select{font:inherit;color:var(--fg);background:#0d1117;border:1px solid var(--line);
 border-radius:10px;padding:10px;width:100%}
input[type=range]{padding:0;border:0;background:transparent;accent-color:var(--acc);height:34px}
label{display:block;font-size:12px;color:var(--dim);margin:0 0 5px;text-transform:uppercase;letter-spacing:.06em}
.lbl{font-size:12px;color:var(--dim);text-transform:uppercase;letter-spacing:.06em;margin-bottom:8px}
.warn{background:#3d1d1d;border-color:#7a2c2c;color:#ffb4b4}
.foot{color:var(--dim);font-size:12px;text-align:center;padding-bottom:20px}
.hide{display:none}
</style></head><body>

<h1>FM Radio</h1>
<div id="warn" class="card warn hide">RDA5807M not detected on the I2C bus - check wiring.</div>

<div class="card">
  <div class="freq"><span id="f">--.-</span><span>MHz</span></div>
  <div class="name" id="nm"></div>
  <div class="meta">
    <span><i class="dot" id="std"></i>Stereo</span>
    <span>Signal</span><span class="bar"><i id="sig" style="width:0"></i></span>
  </div>
</div>

<div class="card">
  <div class="lbl">Volume <span id="vv">-</span>/15</div>
  <input type="range" id="vol" min="0" max="15" value="0">
  <div class="row" style="margin-top:12px">
    <button onclick="api('/api/seek?dir=down')">&#9664;&#9664; Seek</button>
    <button onclick="api('/api/seek?dir=up')">Seek &#9654;&#9654;</button>
  </div>
  <div class="row" style="margin-top:8px">
    <input id="mf" type="number" step="0.1" min="87" max="108" placeholder="98.3">
    <button class="acc" style="flex:0 0 90px" onclick="tune()">Tune</button>
  </div>
</div>

<div class="card">
  <div class="row" style="margin-bottom:10px">
    <div class="lbl" style="margin:0">Presets</div>
    <button style="flex:0 0 90px;padding:6px" id="ed" onclick="toggleEdit()">Edit</button>
  </div>
  <div class="grid" id="pg"></div>
  <div id="editor" class="hide" style="margin-top:12px;border-top:1px solid var(--line);padding-top:12px">
    <label>Slot <span id="es"></span> name</label>
    <input id="en" maxlength="12">
    <label style="margin-top:10px">Frequency (MHz)</label>
    <input id="ef" type="number" step="0.1" min="87" max="108">
    <div class="row" style="margin-top:10px">
      <button onclick="closeEd()">Cancel</button>
      <button class="acc" onclick="saveEd()">Save preset</button>
    </div>
  </div>
</div>

<div class="card">
  <label for="ol">OLED display</label>
  <select id="ol" onchange="api('/api/oled?type='+this.value)">
    <option value="off">Off</option>
    <option value="ssd1306">SSD1306</option>
    <option value="sh1106">SH1106</option>
  </select>
  <div class="meta" id="olm"></div>
</div>

<div class="card" id="fwcard">
  <div class="lbl">Firmware</div>
  <div class="meta" id="fw"></div>
  <button style="width:100%;margin-top:12px" onclick="location.href='/update'">
    Update firmware over the air
  </button>
</div>

<div class="foot" id="ft"></div>

<script>
var S=null,edit=false,slot=-1,hold=0;

function api(u){return fetch(u).then(function(){return poll()})}
function poll(){
  return fetch('/api/status').then(function(r){return r.json()}).then(function(j){S=j;draw()}).catch(function(){})
}
function draw(){
  if(!S)return;
  document.getElementById('f').textContent=S.freqStr;
  document.getElementById('nm').textContent=S.preset<0?'Manual tune':S.presets[S.preset].n;
  document.getElementById('std').className='dot'+(S.stereo?' on':'');
  document.getElementById('sig').style.width=Math.min(100,Math.round(S.rssi*100/64))+'%';
  document.getElementById('warn').className='card warn'+(S.radio?' hide':'');
  if(document.activeElement!==document.getElementById('vol')&&Date.now()>hold){
    document.getElementById('vol').value=S.vol
  }
  document.getElementById('vv').textContent=S.vol;
  var g=document.getElementById('pg'),h='';
  for(var i=0;i<S.presets.length;i++){
    h+='<button class="'+(i===S.preset?'sel':'')+'" onclick="hit('+i+')">'+
       esc(S.presets[i].n)+'<b>'+(S.presets[i].f/100).toFixed(1)+'</b></button>'
  }
  g.innerHTML=h;
  document.getElementById('ol').value=['off','ssd1306','sh1106'][S.oled];
  document.getElementById('olm').textContent=S.oledFound?'Panel detected on the I2C bus.':
    'No panel on the bus - the radio runs fine without one.';
  document.getElementById('fw').textContent='Built '+S.build;
  document.getElementById('fwcard').className='card'+(S.ota?'':' hide');
  document.getElementById('ft').textContent=S.board+' · '+S.ip+' · '+S.host+'.local';
}
function esc(s){return s.replace(/[&<>]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;'}[c]})}
function hit(i){ if(edit)openEd(i); else api('/api/preset?i='+i) }
function toggleEdit(){
  edit=!edit;
  document.getElementById('ed').className=edit?'acc':'';
  if(!edit)closeEd()
}
function openEd(i){
  slot=i;
  document.getElementById('es').textContent=i+1;
  document.getElementById('en').value=S.presets[i].n;
  document.getElementById('ef').value=(S.presets[i].f/100).toFixed(1);
  document.getElementById('editor').className=''
}
function closeEd(){ slot=-1; document.getElementById('editor').className='hide' }
function saveEd(){
  if(slot<0)return;
  var f=Math.round(parseFloat(document.getElementById('ef').value)*100);
  if(!(f>=8700&&f<=10800)){alert('Frequency must be between 87.0 and 108.0');return}
  api('/api/preset/set?i='+slot+'&freq='+f+'&name='+
      encodeURIComponent(document.getElementById('en').value)).then(closeEd)
}
function tune(){
  var f=Math.round(parseFloat(document.getElementById('mf').value)*100);
  if(!(f>=8700&&f<=10800)){alert('Frequency must be between 87.0 and 108.0');return}
  api('/api/tune?freq='+f)
}
var v=document.getElementById('vol');
v.addEventListener('input',function(){
  hold=Date.now()+1500;
  document.getElementById('vv').textContent=v.value;
  clearTimeout(v._t); v._t=setTimeout(function(){api('/api/volume?v='+v.value)},120)
});
poll(); setInterval(poll,1000);
</script>
</body></html>)HTML";

static char jsonBuf[1024];

static void buildStatusJson() {
  char fbuf[10];
  freqToStr(cfg.freq10k, fbuf, sizeof(fbuf));

#if OLED_ENABLED
  const bool oledFound = oledActive;
#else
  const bool oledFound = false;
#endif

  String ip = wifiUp() ? WiFi.localIP().toString() : String("0.0.0.0");

  int n = snprintf(jsonBuf, sizeof(jsonBuf),
    "{\"board\":\"%s\",\"radio\":%s,\"freq\":%u,\"freqStr\":\"%s\",\"preset\":%d,"
    "\"vol\":%u,\"rssi\":%u,\"stereo\":%s,\"tuned\":%s,\"oled\":%u,\"oledFound\":%s,"
    "\"wifi\":%s,\"ip\":\"%s\",\"host\":\"%s\",\"ota\":%s,\"build\":\"%s\","
    "\"presets\":[",
    BOARD_NAME,
    radio.present() ? "true" : "false",
    (unsigned)cfg.freq10k, fbuf,
    (cfg.presetIdx < PRESET_COUNT) ? (int)cfg.presetIdx : -1,
    (unsigned)cfg.volume,
    (unsigned)radio.rssi(),
    radio.stereo() ? "true" : "false",
    radio.tuned()  ? "true" : "false",
    (unsigned)cfg.oledType,
    oledFound ? "true" : "false",
    wifiUp() ? "true" : "false",
    ip.c_str(), MDNS_HOSTNAME,
    OTA_ENABLED ? "true" : "false",
    FW_BUILD);

  if (n < 0) n = 0;
  for (uint8_t i = 0; i < PRESET_COUNT && n < (int)sizeof(jsonBuf) - 2; i++) {
    int w = snprintf(jsonBuf + n, sizeof(jsonBuf) - n, "%s{\"f\":%u,\"n\":\"%s\"}",
                     i ? "," : "", (unsigned)cfg.presets[i].freq10k, cfg.presets[i].name);
    if (w < 0) break;
    n += w;
    if (n >= (int)sizeof(jsonBuf)) { n = (int)sizeof(jsonBuf) - 1; break; }
  }
  snprintf(jsonBuf + n, sizeof(jsonBuf) - n, "]}");
}

static void sendStatus() {
  buildStatusJson();
  server.send(200, F("application/json"), jsonBuf);
}

// Copies a user-supplied preset name into the settings, keeping only printable
// ASCII and dropping the two characters that would need JSON escaping.
static void copyPresetName(const String &src, char *dst) {
  uint8_t o = 0;
  for (uint16_t i = 0; i < src.length() && o < PRESET_NAME_LEN - 1; i++) {
    char c = src[i];
    if (c < 32 || c > 126 || c == '"' || c == '\\') continue;
    dst[o++] = c;
  }
  dst[o] = '\0';
  if (o == 0) strcpy(dst, "Preset");
}

// ===========================================================================
//  8. OTA FIRMWARE UPDATE
// ===========================================================================
//
//  The board is in a sealed enclosure, so there are two independent ways in.
//  Both are protected by OTA_PASSWORD.
//
//    1. Arduino IDE / arduino-cli network port. The board advertises an
//       Arduino OTA service over mDNS, so it shows up as a network port:
//         Arduino IDE -> Tools -> Port -> "radio at 192.168.x.x"
//         arduino-cli upload -p radio.local --fqbn <fqbn> \
//                     --upload-field password=fmradio <sketch-dir>
//
//    2. Browser upload at http://radio.local/update -- choose a compiled
//       .bin and hit Upload. No toolchain needed, works from a phone.
//       Export the .bin with: arduino-cli compile --export-binaries ...
//
//  The RDA5807M keeps playing throughout: it only needs the MCU to change
//  station, so audio does not drop while the flash is being written.
//
//  Recovery: if an update fails mid-transfer the bootloader still holds the
//  old firmware and the board comes back on it. Only a *successfully written
//  but broken* sketch needs the box opened, so test over USB before pushing.

// Declared unconditionally so loop() can guard on them either way.
static bool          otaActive     = false;
static uint8_t       otaLastPct    = 255;   // last percent drawn (IDE path)
static uint32_t      otaLastBlock  = 0;     // last 20 KB block drawn (web path)
static unsigned long otaLastSeen   = 0;     // for the stall timeout below

#if OTA_ENABLED

// Full-screen progress display. pct < 0 draws the title only.
static void otaScreen(const char *title, int pct) {
#if OLED_ENABLED
  if (!oledActive) return;
  char buf[8];
  oledClear();
  gfx->setTextColor(OLED_ON);
  gfx->setTextSize(1);
  gfx->setCursor(0, 4);
  gfx->print(title);
  if (pct >= 0) {
    snprintf(buf, sizeof(buf), "%d%%", pct);
    gfx->setTextSize(3);
    gfx->setCursor(0, 22);
    gfx->print(buf);
    gfx->drawRect(0, 52, 128, 8, OLED_ON);
    if (pct > 0) gfx->fillRect(2, 54, (int16_t)((124 * pct) / 100), 4, OLED_ON);
  }
  oledShow();
#else
  (void)title; (void)pct;
#endif
}

// Throttled to every 5% so redrawing the OLED never slows the transfer down.
static void otaProgress(uint32_t done, uint32_t total) {
  if (!total) return;
  uint8_t pct = (uint8_t)((uint64_t)done * 100 / total);
  if (pct > 100) pct = 100;
  if (pct == otaLastPct) return;
  if ((pct % 5) && pct != 100) return;
  otaLastPct  = pct;
  otaLastSeen = millis();
  Serial.printf("[ota] %u%%\n", (unsigned)pct);
  otaScreen("Updating firmware", pct);
}

// --- browser upload --------------------------------------------------------

static const char UPDATE_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FM Radio - Firmware Update</title>
<style>
:root{--bg:#0d1117;--card:#161b22;--line:#272e38;--fg:#e6edf3;--dim:#8b949e;--acc:#f0883e;--ok:#3fb950;--bad:#f85149}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.4 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;
 padding:16px;max-width:520px;margin-inline:auto}
h1{font-size:15px;font-weight:600;letter-spacing:.08em;text-transform:uppercase;color:var(--dim);margin:0 0 12px}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px;margin-bottom:12px}
p{color:var(--dim);font-size:13px;margin:0 0 14px}
input[type=file]{font:inherit;color:var(--fg);width:100%;margin-bottom:12px}
input[type=file]::file-selector-button{font:inherit;color:var(--fg);background:#21262d;border:1px solid var(--line);
 border-radius:9px;padding:9px 12px;margin-right:10px;cursor:pointer}
button{font:inherit;color:#111;background:var(--acc);border:0;border-radius:10px;padding:12px;width:100%;
 font-weight:600;cursor:pointer}
button:disabled{opacity:.5;cursor:default}
a{color:var(--dim);font-size:13px}
.bar{height:8px;background:#21262d;border-radius:4px;overflow:hidden;margin-top:14px}
.bar i{display:block;height:100%;width:0;background:var(--acc);transition:width .15s}
#msg{margin-top:12px;font-size:14px}
.ok{color:var(--ok)} .bad{color:var(--bad)}
</style></head><body>
<h1>Firmware Update</h1>
<div class="card">
  <p>Pick a compiled <b>.bin</b> for this board. Do not power the radio off while
     it uploads. If the transfer is interrupted the current firmware keeps running.</p>
  <input type="file" id="f" accept=".bin">
  <button id="go" onclick="up()">Upload &amp; reboot</button>
  <div class="bar"><i id="p"></i></div>
  <div id="msg"></div>
</div>
<a href="/">&larr; Back to the radio</a>
<script>
function up(){
  var f=document.getElementById('f').files[0];
  if(!f){msg('Choose a .bin file first.','bad');return}
  var d=new FormData(); d.append('firmware',f,f.name);
  var x=new XMLHttpRequest();
  x.upload.onprogress=function(e){
    if(e.lengthComputable)document.getElementById('p').style.width=(e.loaded/e.total*100)+'%'
  };
  x.onload=function(){
    if(x.status===200){
      msg('Update accepted. Rebooting - returning to the radio...','ok');
      setTimeout(function(){location.href='/'},12000)
    } else msg('Update failed ('+x.status+'). The old firmware is still running.','bad')
  };
  x.onerror=function(){msg('Connection lost during upload.','bad')};
  x.open('POST','/update');
  x.send(d);
  document.getElementById('go').disabled=true;
  msg('Uploading '+Math.round(f.size/1024)+' KB...','')
}
function msg(t,c){var m=document.getElementById('msg');m.textContent=t;m.className=c}
</script>
</body></html>)HTML";

static bool otaAuthOk() {
  if (!strlen(OTA_PASSWORD)) return true;
  return server.authenticate(OTA_USERNAME, OTA_PASSWORD);
}

static void handleUpdatePage() {
  if (!otaAuthOk()) return server.requestAuthentication();
  server.send_P(200, PSTR("text/html"), UPDATE_HTML);
}

// Runs once the whole body has been received.
static void handleUpdateDone() {
  if (!otaAuthOk()) return server.requestAuthentication();
  bool ok = !Update.hasError();
  server.sendHeader(F("Connection"), F("close"));
  server.send(ok ? 200 : 500, F("text/plain"),
              ok ? F("OK - rebooting") : F("FAILED - old firmware kept"));
  if (ok) {
    delay(300);
    ESP.restart();
  }
  otaActive = false;
}

// Called repeatedly with chunks of the multipart body.
static void handleUpdateUpload() {
  static bool authed = false;
  HTTPUpload &up = server.upload();

  if (up.status == UPLOAD_FILE_START) {
    authed = otaAuthOk();
    if (!authed) return;
    otaActive    = true;
    otaLastBlock = 0;
    otaLastSeen  = millis();
    Serial.printf("[ota] web upload: %s\n", up.filename.c_str());
    otaScreen("Updating firmware", 0);
#if defined(ESP8266)
    WiFiUDP::stopAll();                       // frees RAM and stops mDNS traffic
    Update.runAsync(true);
    uint32_t room = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    if (!Update.begin(room)) Update.printError(Serial);
#else
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
#endif

  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!authed) return;
    if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
    otaLastSeen = millis();
    // The browser draws the accurate bar; the OLED just counts KB written.
    if (up.totalSize / 20480 != otaLastBlock) {
      otaLastBlock = up.totalSize / 20480;
      char t[26];
      snprintf(t, sizeof(t), "Updating  %u KB", (unsigned)(up.totalSize / 1024));
      otaScreen(t, -1);
    }

  } else if (up.status == UPLOAD_FILE_END) {
    if (!authed) return;
    if (Update.end(true)) {
      Serial.printf("[ota] %u bytes written, rebooting\n", (unsigned)up.totalSize);
      otaScreen("Done - rebooting", 100);
    } else {
      Update.printError(Serial);
      otaScreen("Update FAILED", -1);
    }

  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.end();
    otaActive = false;
    Serial.println(F("[ota] upload aborted"));
    otaScreen("Update aborted", -1);
  }
}

// --- IDE / arduino-cli network port ----------------------------------------
// Called once Wi-Fi and mDNS are up (see networkTick).

static void otaBegin() {
  ArduinoOTA.setHostname(MDNS_HOSTNAME);
  if (strlen(OTA_PASSWORD)) ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    otaActive  = true;
    otaLastPct = 255;
    Serial.println(F("[ota] IDE update starting"));
    otaScreen("Updating firmware", 0);
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    otaProgress((uint32_t)done, (uint32_t)total);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println(F("[ota] done, rebooting"));
    otaScreen("Done - rebooting", 100);
  });
  ArduinoOTA.onError([](ota_error_t err) {
    otaActive = false;
    Serial.printf("[ota] error %u\n", (unsigned)err);
    otaScreen("Update FAILED", -1);
  });

  // mDNS is already running (networkTick started it), so tell ArduinoOTA not
  // to restart it and just advertise the Arduino service on the existing one.
#if defined(ESP8266)
  ArduinoOTA.begin(false);
  MDNS.enableArduino(8266, strlen(OTA_PASSWORD) > 0);
#else
  ArduinoOTA.setMdnsEnabled(false);
  ArduinoOTA.begin();
  MDNS.enableArduino(3232, strlen(OTA_PASSWORD) > 0);
#endif

  Serial.println(F("[ota] IDE network port ready + http://" MDNS_HOSTNAME ".local/update"));
}

static inline void otaTick() { ArduinoOTA.handle(); }

#else   // OTA_ENABLED == 0
static inline void otaBegin() {}
static inline void otaTick()  {}
#endif

// ===========================================================================
//  9. WEB ROUTES
// ===========================================================================

static void routes() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, PSTR("text/html"), INDEX_HTML);
  });

  server.on("/api/status", HTTP_GET, sendStatus);

  server.on("/api/preset", HTTP_GET, []() {
    if (server.hasArg("i")) applyPreset((uint8_t)server.arg("i").toInt());
    sendStatus();
  });

  server.on("/api/tune", HTTP_GET, []() {
    if (server.hasArg("freq")) applyFrequency((uint16_t)server.arg("freq").toInt(), PRESET_MANUAL);
    sendStatus();
  });

  server.on("/api/volume", HTTP_GET, []() {
    if (server.hasArg("v")) applyVolume((uint8_t)server.arg("v").toInt());
    sendStatus();
  });

  server.on("/api/seek", HTTP_GET, []() {
    bool up = !server.hasArg("dir") || server.arg("dir") != "down";
    uint16_t landed = radio.seek(up);
    applyFrequency(landed, PRESET_MANUAL);
    sendStatus();
  });

  server.on("/api/preset/set", HTTP_GET, []() {
    if (server.hasArg("i")) {
      int i = server.arg("i").toInt();
      if (i >= 0 && i < PRESET_COUNT) {
        if (server.hasArg("freq"))
          cfg.presets[i].freq10k = RDA5807M::clampFreq((uint16_t)server.arg("freq").toInt());
        if (server.hasArg("name"))
          copyPresetName(server.arg("name"), cfg.presets[i].name);
        settingsTouch();
        // If the slot being edited is the one playing, follow the new setting.
        if (cfg.presetIdx == i) applyPreset((uint8_t)i); else oledRender();
      }
    }
    sendStatus();
  });

  server.on("/api/oled", HTTP_GET, []() {
    if (server.hasArg("type")) {
      String t = server.arg("type");
      uint8_t want = (t == "ssd1306") ? OLED_TYPE_SSD1306
                   : (t == "sh1106")  ? OLED_TYPE_SH1106
                                      : OLED_TYPE_NONE;
      cfg.oledType = want;
      settingsTouch();
      oledBegin(want);
      oledRender();
    }
    sendStatus();
  });

#if OTA_ENABLED
  // Browser firmware upload. The POST takes two callbacks: the second eats
  // the multipart body chunk by chunk, the first replies once it is done.
  server.on("/update", HTTP_GET,  handleUpdatePage);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
#endif

  server.onNotFound([]() {
    server.send(404, F("application/json"), F("{\"error\":\"not found\"}"));
  });
}

// ===========================================================================
//  10. NETWORK
// ===========================================================================

static void wifiBegin() {
  WiFi.mode(WIFI_STA);
#if defined(ESP8266)
  WiFi.hostname(MDNS_HOSTNAME);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);     // keeps the web UI snappy
#else
  WiFi.setHostname(MDNS_HOSTNAME);
  WiFi.setSleep(false);
#endif
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[wifi] connecting to \"%s\"", WIFI_SSID);
}

// Called from loop(). Starts mDNS on the first successful connection and
// retries the association if the router goes away, without ever blocking the
// radio or the button.
static void networkTick() {
  static unsigned long lastRetry = 0;
  static bool wasUp = false;

  bool up = wifiUp();

  if (up && !wasUp) {
    Serial.printf("\n[wifi] connected, IP %s\n", WiFi.localIP().toString().c_str());
    if (!mdnsStarted && MDNS.begin(MDNS_HOSTNAME)) {
      MDNS.addService("http", "tcp", 80);
      mdnsStarted = true;
      Serial.println(F("[mdns] http://" MDNS_HOSTNAME ".local"));
    }
    // OTA needs an association, so it starts here rather than in setup().
    if (!otaStarted) { otaBegin(); otaStarted = true; }
    oledRender();
  } else if (!up && wasUp) {
    Serial.println(F("[wifi] lost connection"));
    oledRender();
  }
  wasUp = up;

  if (!up && millis() - lastRetry > 15000) {
    lastRetry = millis();
    Serial.println(F("[wifi] retrying"));
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

// ===========================================================================
//  11. BUTTON
// ===========================================================================

static void buttonTick() {
  static bool          lastRaw   = true;    // pulled up == released
  static bool          stable    = true;
  static unsigned long lastEdge  = 0;

  bool raw = digitalRead(PIN_BUTTON);
  if (raw != lastRaw) { lastRaw = raw; lastEdge = millis(); return; }

  if (millis() - lastEdge < 50) return;     // still bouncing
  if (raw == stable) return;

  stable = raw;
  if (!stable) nextPreset();                // act on press, not release
}

// ===========================================================================
//  12. SETUP / LOOP
// ===========================================================================

static void i2cScan() {
  Serial.println(F("[i2c] scanning..."));
  uint8_t found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { Serial.printf("[i2c]   0x%02X\n", a); found++; }
  }
  if (!found) Serial.println(F("[i2c]   nothing responded - check SDA/SCL and pull-ups"));
}

void setup() {
  Serial.begin(115200);
  delay(300);                               // let USB-CDC enumerate on the C3
  Serial.println();
  Serial.println(F("==================================="));
  Serial.println(F(" RDA5807M FM Radio - " BOARD_NAME));
  Serial.println(F("==================================="));

  // On the ESP8266 this steals GPIO3 from UART0 RX; Serial output still works.
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);                    // the RDA5807M is happier at 100 kHz
  i2cScan();

  settingsLoad();

  // Radio first, so audio is playing before the network is even attempted.
  if (radio.begin()) {
    Serial.println(F("[radio] RDA5807M ready"));
    radio.setVolume(cfg.volume);
    radio.setFrequency(cfg.freq10k);
    radio.readStatus(true);
  } else {
    Serial.println(F("[radio] RDA5807M NOT found - continuing so the web UI is still reachable"));
  }

  oledBegin(cfg.oledType);
  oledRender();

  wifiBegin();
  routes();
  server.begin();
  Serial.println(F("[http] server started on port 80"));

#if OTA_ENABLED
  // An OTA image has to fit in the free space alongside the running sketch.
  // If "free" here is smaller than the .bin you are pushing, OTA cannot work
  // and the flash-size / partition setting for the board needs changing.
  Serial.printf("[ota] sketch %u bytes, free for update %u bytes\n",
                (unsigned)ESP.getSketchSize(), (unsigned)ESP.getFreeSketchSpace());
  Serial.println(F("[ota] build " FW_BUILD));
#endif
}

void loop() {
  server.handleClient();
  otaTick();
#if defined(ESP8266)
  if (mdnsStarted) MDNS.update();
#endif

  // While firmware is being written, keep off the I2C bus and out of the
  // settings flash area -- the update owns the chip until it reboots.
  if (otaActive) {
    // A browser tab closed mid-upload never fires UPLOAD_FILE_ABORTED, so
    // without this the box would need a power cycle to become usable again.
    if (millis() - otaLastSeen > 30000) {
      otaActive = false;
      Serial.println(F("[ota] stalled, giving up and resuming normal operation"));
      oledRender();
    }
    return;
  }

  networkTick();
  buttonTick();
  settingsTick();

  radio.readStatus();                       // rate-limited internally

  static unsigned long lastDraw = 0;
  if (millis() - lastDraw >= 1000) {        // refresh RSSI / stereo on the OLED
    lastDraw = millis();
    oledRender();
  }
}
                                                                                 /// End.
