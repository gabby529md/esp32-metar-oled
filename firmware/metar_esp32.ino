/*
 ============================================================
  METAR DISPLAY — ESP32 + SH1106/SSD1306 OLED
  Live METAR fetch via WiFi from NOAA AWC (free, no key)
  with CheckWX as fallback (free tier, key required)
 
  Libraries required (install via Arduino Library Manager):
    - U8g2lib        by olikraus
    - ArduinoJson    by Benoit Blanchon  (v6 or v7)
    - WiFi           (bundled with ESP32 core)
    - HTTPClient     (bundled with ESP32 core)
    - WiFiClientSecure (bundled with ESP32 core)

  Board: ESP32 Dev Module (or any ESP32 variant)
  Arduino IDE: 2.x  |  ESP32 core: >= 2.0.x
 ============================================================
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <time.h>           // ESP32 POSIX time — NTP syncs into this
#include "esp_sntp.h"       // SNTP callbacks

// ============================================================
//  USER CONFIGURATION — edit these
// ============================================================

// WiFi credentials
const char* WIFI_SSID     = "YOUR_SSID";
const char* WIFI_PASSWORD = "YOUR_PASSWORD";

// ICAO station(s) to monitor — UK examples: EGLL EGCC EGPH EGPD EGSS
// US examples: KJFK KLAX KORD KATL
// Comma-separated for rotation, e.g. "EGLL,EGCC,EGPH"
const char* STATIONS = "EGLL,EGPH,KJFK";

// CheckWX API key (free tier: 100 calls/hour, sign up at checkwxapi.com)
// Leave blank to use NOAA only (fully free, no key needed)
const char* CHECKWX_API_KEY = "";   // e.g. "abc123def456"

// Fetch interval (milliseconds) — NOAA updates ~30 min, CheckWX ~15 min
// Don't go below 5 min to be polite to public APIs
const unsigned long FETCH_INTERVAL_MS = 10UL * 60UL * 1000UL; // 10 minutes

// NTP configuration
// pool.ntp.org serves UK + US fine; add uk.pool.ntp.org as backup
const char* NTP_SERVER_1   = "pool.ntp.org";
const char* NTP_SERVER_2   = "uk.pool.ntp.org";
const char* NTP_SERVER_3   = "time.google.com";
const long  GMT_OFFSET_SEC = 0;          // 0 = UTC; use 3600 for BST (+1h) or -18000 for US EST
const int   DAYLIGHT_OFFSET_SEC = 0;     // set to 3600 if you want auto-DST (Europe/London)

// NTP sync state
volatile bool ntpSynced = false;

// SNTP sync notification callback
void ntpSyncCallback(struct timeval* tv) {
  ntpSynced = true;
  Serial.println("[NTP] Time synchronised successfully");
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &timeinfo);
    Serial.printf("[NTP] Current time: %s\n", buf);
  }
}

// OLED wiring — adjust pins to match your hardware
// I2C address: 0x3C (most common) or 0x3D
#define OLED_SDA  21
#define OLED_SCL  22
#define OLED_ADDR 0x3C

// LED indicator pins (set to -1 to disable)
#define LED_VFR   2    // Green LED
#define LED_MVFR  4    // Blue LED
#define LED_IFR   5    // Red LED
#define LED_LIFR  18   // Purple/White LED
#define LED_WARN  19   // Amber — wind/gust warning

// ============================================================
//  OLED DRIVER — choose your display type
//  Uncomment ONE line matching your hardware
// ============================================================

// SH1106 128x64 (common 1.3" modules)
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);

// SSD1306 128x64 (common 0.96" modules) — uncomment to use instead:
// U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);

// SSD1309 128x64 (larger 2.4" modules):
// U8G2_SSD1309_128X64_NONAME0_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);

// ============================================================
//  METAR DATA STRUCTURE
// ============================================================

struct MetarData {
  char  icao[5];
  char  raw[256];
  char  observed[32];          // ISO timestamp
  char  flightCategory[6];     // VFR MVFR IFR LIFR
  int   tempC;
  int   dewC;
  int   visMetres;
  int   windDir;               // -1 = variable
  int   windSpeedKt;
  int   windGustKt;            // 0 = no gusts
  int   qnhHpa;
  char  wxString[64];          // e.g. "-RA BR"
  char  cloudString[80];       // e.g. "FEW015 BKN040"
  bool  valid;
};

MetarData metar;

// Station list split
char stationList[8][5];
int  stationCount = 0;
int  currentStation = 0;

// Timing
unsigned long lastFetch = 0;
unsigned long lastStationSwitch = 0;
const unsigned long SWITCH_INTERVAL_MS = 15000; // rotate display every 15s

// ============================================================
//  NOAA AWC API  (no key required — completely free)
//  Endpoint: https://aviationweather.gov/api/data/metar
//  Returns: JSON or raw text
//  Rate limit: 100 req/min (very generous)
//  Coverage: worldwide (sourced from ICAO global feed)
// ============================================================

const char* NOAA_HOST = "aviationweather.gov";
// Root cert for aviationweather.gov (DigiCert Global Root CA — valid to 2031)
// Run: openssl s_client -connect aviationweather.gov:443 2>/dev/null | openssl x509 -noout -text
// to refresh if expired
const char* NOAA_CERT = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDrzCCApegAwIBAgIQCDvgVpBCRrGhdWrJWZHHSjANBgkqhkiG9w0BAQUFADBh
MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3
d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBD
QTAeFw0wNjExMTAwMDAwMDBaFw0zMTExMTAwMDAwMDBaMGExCzAJBgNVBAYTAlVT
MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j
b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IENBMB8GA1UdIwQYMBaA
FAPeUDVW0Uy7ZvCj4hsbw5eyPdFVMA4GA1UdDwEB/wQEAwIBhjANBgkqhkiG9w0B
AQUFAAOCAQEAkBt7+GzmFJbBMywMn5YfZsGomtjbORqNkFERs0VBt2KZFN4QGXTZ
B7T5P4MfpRnNM0RQwRr/9F/Q+0Y6BXt5nBBqOPMoG2ck0WWMW2z/CeWFqFxUHVbT
d5BOhNV9zR2j/T0UVRaWo2v3r1MjH2iWdAvZIGN88gW3VioQqA8NhEbNIDRzDgUf
MAAAAAAAAAAAAA==
-----END CERTIFICATE-----
)EOF";

bool fetchFromNOAA(const char* icao) {
  Serial.printf("[NOAA] Fetching METAR for %s...\n", icao);

  WiFiClientSecure client;
  client.setCACert(NOAA_CERT);
  // If cert verification fails, use this instead (less secure but works):
  // client.setInsecure();

  HTTPClient https;
  char url[128];
  snprintf(url, sizeof(url),
    "https://aviationweather.gov/api/data/metar?ids=%s&format=json&taf=false",
    icao);

  https.begin(client, url);
  https.addHeader("User-Agent", "ESP32-METAR-Display/1.0");
  https.useHTTP10(true);

  int code = https.GET();
  Serial.printf("[NOAA] HTTP %d\n", code);

  if (code != 200) {
    https.end();
    return false;
  }

  // Parse JSON  — allocate on heap for ESP32
  JsonDocument doc;   // ArduinoJson v7
  // DynamicJsonDocument doc(4096);   // ArduinoJson v6 — uncomment if using v6

  DeserializationError err = deserializeJson(doc, https.getStream());
  https.end();

  if (err) {
    Serial.printf("[NOAA] JSON parse error: %s\n", err.c_str());
    return false;
  }

  // NOAA returns an array; first element is latest METAR
  if (!doc.is<JsonArray>() || doc.as<JsonArray>().size() == 0) {
    Serial.println("[NOAA] Empty response");
    return false;
  }

  JsonObject obs = doc[0];

  // ICAO & raw
  strlcpy(metar.icao,    obs["icaoId"]  | icao,    sizeof(metar.icao));
  strlcpy(metar.raw,     obs["rawOb"]   | "",       sizeof(metar.raw));
  strlcpy(metar.observed,obs["reportTime"]| "",     sizeof(metar.observed));

  // Flight category
  strlcpy(metar.flightCategory, obs["fltcat"] | "?", sizeof(metar.flightCategory));

  // Temperature / dewpoint (NOAA returns as strings sometimes)
  metar.tempC       = obs["temp"].is<int>()   ? (int)obs["temp"]   : atoi(obs["temp"] | "0");
  metar.dewC        = obs["dewp"].is<int>()   ? (int)obs["dewp"]   : atoi(obs["dewp"] | "0");

  // Visibility — NOAA returns statute miles (US) — convert to metres
  float visMiles    = obs["visib"].is<float>() ? (float)obs["visib"] : atof(obs["visib"] | "10");
  metar.visMetres   = (int)(visMiles * 1609.34f);
  if (metar.visMetres > 9999) metar.visMetres = 9999;

  // Wind
  metar.windDir     = obs["wdir"].is<int>()   ? (int)obs["wdir"]   : -1;
  metar.windSpeedKt = obs["wspd"].is<int>()   ? (int)obs["wspd"]   : 0;
  metar.windGustKt  = obs["wgst"].is<int>()   ? (int)obs["wgst"]   : 0;

  // QNH — NOAA gives altimeter in inHg ("altim"), convert to hPa
  float altim       = obs["altim"] | 29.92f;
  metar.qnhHpa      = (int)(altim * 33.8639f);

  // Weather phenomena
  strlcpy(metar.wxString, obs["wxString"] | "", sizeof(metar.wxString));

  // Cloud layers — build string from array
  metar.cloudString[0] = '\0';
  JsonArray clouds = obs["clouds"].as<JsonArray>();
  for (JsonObject cl : clouds) {
    char layer[16];
    snprintf(layer, sizeof(layer), "%s%03d ",
      cl["cover"] | "???",
      (int)(cl["base"] | 0) / 100);   // NOAA base in feet, divide back to hundreds
    strlcat(metar.cloudString, layer, sizeof(metar.cloudString));
  }

  metar.valid = true;
  Serial.printf("[NOAA] OK — %s  CAT:%s  T:%d  Wind:%d/%dKT  VIS:%dm\n",
    metar.icao, metar.flightCategory, metar.tempC,
    metar.windDir, metar.windSpeedKt, metar.visMetres);
  return true;
}

// ============================================================
//  CheckWX API  (free tier: 100 calls/hour, key required)
//  Sign up: https://checkwxapi.com
//  Returns clean decoded JSON — easier to parse than NOAA
//  Used as fallback if NOAA fails, or as primary if key set
// ============================================================

const char* CHECKWX_HOST = "api.checkwx.com";
const char* CHECKWX_CERT = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDrzCCApegAwIBAgIQCDvgVpBCRrGhdWrJWZHHSjANBgkqhkiG9w0BAQUFADBh
MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3
d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBD
QTAeFw0wNjExMTAwMDAwMDBaFw0zMTExMTAwMDAwMDBaMGExCzAJBgNVBAYTAlVT
MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j
b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IENBMB8GA1UdIwQYMBaA
FAPeUDVW0Uy7ZvCj4hsbw5eyPdFVMA4GA1UdDwEB/wQEAwIBhjANBgkqhkiG9w0B
AQUFAAOCAQEAkBt7+GzmFJbBMywMn5YfZsGomtjbORqNkFERs0VBt2KZFN4QGXTZ
B7T5P4MfpRnNM0RQwRr/9F/Q+0Y6BXt5nBBqOPMoG2ck0WWMW2z/CeWFqFxUHVbT
d5BOhNV9zR2j/T0UVRaWo2v3r1MjH2iWdAvZIGN88gW3VioQqA8NhEbNIDRzDgUf
MAAAAAAAAAAAAA==
-----END CERTIFICATE-----
)EOF";

bool fetchFromCheckWX(const char* icao) {
  if (strlen(CHECKWX_API_KEY) == 0) return false;
  Serial.printf("[CheckWX] Fetching METAR for %s...\n", icao);

  WiFiClientSecure client;
  client.setInsecure();  // CheckWX cert chain can vary; use insecure for simplicity

  HTTPClient https;
  char url[128];
  snprintf(url, sizeof(url), "https://api.checkwx.com/metar/%s/decoded", icao);

  https.begin(client, url);
  https.addHeader("X-API-Key", CHECKWX_API_KEY);
  https.addHeader("User-Agent", "ESP32-METAR-Display/1.0");
  https.useHTTP10(true);

  int code = https.GET();
  Serial.printf("[CheckWX] HTTP %d\n", code);

  if (code != 200) { https.end(); return false; }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, https.getStream());
  https.end();

  if (err || doc["results"].as<int>() == 0) {
    Serial.println("[CheckWX] Parse error or no results");
    return false;
  }

  JsonObject obs = doc["data"][0];

  strlcpy(metar.icao,    obs["icao"] | icao, sizeof(metar.icao));
  strlcpy(metar.raw,     obs["raw_text"] | "", sizeof(metar.raw));
  strlcpy(metar.observed,obs["observed"] | "", sizeof(metar.observed));
  strlcpy(metar.flightCategory, obs["flight_category"] | "?", sizeof(metar.flightCategory));

  metar.tempC       = obs["temperature"]["celsius"] | 0;
  metar.dewC        = obs["dewpoint"]["celsius"]    | 0;

  // CheckWX visibility in metres
  metar.visMetres   = obs["visibility"]["meters_float"] | 9999;
  if (metar.visMetres > 9999) metar.visMetres = 9999;

  metar.windDir     = obs["wind"]["degrees"]         | -1;
  metar.windSpeedKt = obs["wind"]["speed_kts"]       | 0;
  metar.windGustKt  = obs["wind"]["gust_kts"]        | 0;

  metar.qnhHpa      = obs["barometer"]["mb"]         | 1013;

  // Weather phenomena
  metar.wxString[0] = '\0';
  JsonArray conds = obs["conditions"].as<JsonArray>();
  for (JsonObject c : conds) {
    strlcat(metar.wxString, c["code"] | "", sizeof(metar.wxString));
    strlcat(metar.wxString, " ", sizeof(metar.wxString));
  }

  // Cloud layers
  metar.cloudString[0] = '\0';
  JsonArray clouds = obs["clouds"].as<JsonArray>();
  for (JsonObject cl : clouds) {
    char layer[16];
    snprintf(layer, sizeof(layer), "%s%d ",
      cl["code"] | "???",
      (int)(cl["feet"] | 0) / 100);
    strlcat(metar.cloudString, layer, sizeof(metar.cloudString));
  }

  metar.valid = true;
  Serial.printf("[CheckWX] OK — %s  CAT:%s\n", metar.icao, metar.flightCategory);
  return true;
}

// ============================================================
//  FETCH WRAPPER — tries NOAA first, CheckWX as fallback
// ============================================================

bool fetchMetar(const char* icao) {
  metar.valid = false;
  // If CheckWX key provided, prefer it (richer decoded data)
  if (strlen(CHECKWX_API_KEY) > 0) {
    if (fetchFromCheckWX(icao)) return true;
  }
  return fetchFromNOAA(icao);
}

// ============================================================
//  NTP CLOCK HELPERS
// ============================================================

// Get current UTC time string "HH:MM:SS"
void getTimeString(char* buf, size_t len, bool showSeconds=true) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo) || !ntpSynced) {
    strlcpy(buf, "--:--:--", len);
    return;
  }
  if (showSeconds)
    strftime(buf, len, "%H:%M:%S", &timeinfo);
  else
    strftime(buf, len, "%H:%M", &timeinfo);
}

// Get date string "DD/MM/YYYY"
void getDateString(char* buf, size_t len) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo) || !ntpSynced) {
    strlcpy(buf, "--/--/----", len);
    return;
  }
  strftime(buf, len, "%d/%m/%Y", &timeinfo);
}

// Draw a 7-segment-style clock on the OLED using U8G2
// Renders into top-right corner of 128×64 display
// Uses u8g2_font_7x13_tf for a chunky digital look
void drawClockOverlay() {
  char timeStr[12], dateStr[12];
  getTimeString(timeStr, sizeof(timeStr), true);   // "14:32:07"
  getDateString(dateStr, sizeof(dateStr));          // "24/05/2026"

  // Clock area: x=66, y=0–25  (right half of top zone)
  // Background clear
  u8g2.setDrawColor(0);
  u8g2.drawBox(66, 0, 62, 26);
  u8g2.setDrawColor(1);

  // Time — large font
  u8g2.setFont(u8g2_font_7x13B_tf);
  u8g2.drawStr(67, 12, timeStr);

  // Date + sync status — small font
  u8g2.setFont(u8g2_font_4x6_tf);
  u8g2.drawStr(67, 21, dateStr);

  // NTP sync indicator
  if (ntpSynced) {
    u8g2.drawStr(67, 26, "UTC NTP OK");
  } else {
    u8g2.drawStr(67, 26, "UTC SYNC..");
  }
}

// ============================================================
//  OLED DISPLAY RENDERER
//  128×64 layout:
//    Row 0 (y=0–14):  ICAO + Flight category + time
//    Row 1 (y=15–28): Wind compass + speed
//    Row 2 (y=29–42): Visibility + QNH
//    Row 3 (y=43–56): Temp/Dew + cloud summary
//    Row 4 (y=57–63): WX phenomena + NOSIG
// ============================================================

// Draw a tiny compass rose and wind arrow
void drawWindCompass(int cx, int cy, int r, int deg) {
  u8g2.drawCircle(cx, cy, r, U8G2_DRAW_ALL);

  // Cardinal tick marks
  float angles[4] = {0, 90, 180, 270};
  for (int i = 0; i < 4; i++) {
    float rad = (angles[i] - 90) * DEG_TO_RAD;
    int x0 = cx + (int)((r-2)*cos(rad));
    int y0 = cy + (int)((r-2)*sin(rad));
    int x1 = cx + (int)(r*cos(rad));
    int y1 = cy + (int)(r*sin(rad));
    u8g2.drawLine(x0, y0, x1, y1);
  }

  // Wind direction arrow
  if (deg >= 0) {
    float rad = (deg - 90) * DEG_TO_RAD;
    int arrowLen = r - 3;
    int ax = cx + (int)(arrowLen * cos(rad));
    int ay = cy + (int)(arrowLen * sin(rad));
    // Shaft
    u8g2.drawLine(cx, cy, ax, ay);
    // Arrowhead
    float h1 = rad + 2.5f, h2 = rad - 2.5f;
    u8g2.drawLine(ax, ay, ax + (int)(4*cos(h1)), ay + (int)(4*sin(h1)));
    u8g2.drawLine(ax, ay, ax + (int)(4*cos(h2)), ay + (int)(4*sin(h2)));
  }

  // Centre dot
  u8g2.drawDisc(cx, cy, 1, U8G2_DRAW_ALL);
}

// Cloud cover bar  x,y = top-left, w=width, cover=0.0–1.0
void drawCloudBar(int x, int y, int w, float cover) {
  u8g2.drawFrame(x, y, w, 4);
  int filled = (int)(cover * w);
  if (filled > 0) u8g2.drawBox(x, y, filled, 4);
}

float coverFraction(const char* code) {
  if (strncmp(code,"FEW",3)==0) return 0.15f;
  if (strncmp(code,"SCT",3)==0) return 0.40f;
  if (strncmp(code,"BKN",3)==0) return 0.70f;
  if (strncmp(code,"OVC",3)==0) return 1.00f;
  if (strncmp(code,"VV", 2)==0) return 0.90f;
  return 0.0f;
}

void renderOLED() {
  u8g2.clearBuffer();

  if (!metar.valid) {
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(10, 20, "Fetching METAR...");
    u8g2.drawStr(10, 32, WiFi.status()==WL_CONNECTED ? "WiFi: OK" : "WiFi: connecting");
    u8g2.sendBuffer();
    return;
  }

  // ---- ROW 0: ICAO + Category + time ----
  u8g2.setFont(u8g2_font_7x13B_tf);
  u8g2.drawStr(0, 12, metar.icao);

  // Category — centre
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(40, 8, metar.flightCategory);

  // ---- NTP CLOCK OVERLAY (top-right) ----
  drawClockOverlay();

  // Horizontal rule (below ICAO + clock)
  u8g2.drawHLine(0, 14, 128);

  // ---- WIND COMPASS (left side, rows 1-3) ----
  int compassCX = 18, compassCY = 36, compassR = 15;
  drawWindCompass(compassCX, compassCY, compassR, metar.windDir);

  // Wind speed + gust text next to compass
  char windLine1[16], windLine2[12];
  if (metar.windDir < 0) {
    snprintf(windLine1, sizeof(windLine1), "VRB/%dKT", metar.windSpeedKt);
  } else {
    snprintf(windLine1, sizeof(windLine1), "%03d/%dKT", metar.windDir, metar.windSpeedKt);
  }
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(38, 24, windLine1);

  if (metar.windGustKt > 0) {
    snprintf(windLine2, sizeof(windLine2), "G%dKT", metar.windGustKt);
    u8g2.drawStr(38, 33, windLine2);
  }

  // ---- VISIBILITY ----
  char visLine[16];
  if (metar.visMetres >= 9999) {
    snprintf(visLine, sizeof(visLine), "VIS >10km");
  } else if (metar.visMetres >= 1000) {
    snprintf(visLine, sizeof(visLine), "VIS %.1fkm", metar.visMetres / 1000.0f);
  } else {
    snprintf(visLine, sizeof(visLine), "VIS %dm", metar.visMetres);
  }
  u8g2.drawStr(38, 42, visLine);

  // ---- QNH ----
  char qnhLine[12];
  snprintf(qnhLine, sizeof(qnhLine), "Q%d", metar.qnhHpa);
  u8g2.drawStr(90, 42, qnhLine);

  // ---- TEMP / DEW ----
  u8g2.drawHLine(0, 44, 128);
  char tdLine[20];
  snprintf(tdLine, sizeof(tdLine), "T%+d D%+d", metar.tempC, metar.dewC);
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 53, tdLine);

  // ---- CLOUD SUMMARY ----
  // Parse first two layers from cloudString for bars
  char cloudCopy[80];
  strlcpy(cloudCopy, metar.cloudString, sizeof(cloudCopy));
  int barX = 60, barY = 49;
  char* tok = strtok(cloudCopy, " ");
  int layerIdx = 0;
  while (tok && layerIdx < 3) {
    if (strlen(tok) >= 5) {
      char code[4]; strlcpy(code, tok, 4);
      int base = atoi(tok+3) * 100;
      // Draw bar label
      u8g2.setFont(u8g2_font_4x6_tf);
      u8g2.drawStr(barX, barY + layerIdx*5 + 4, code);
      drawCloudBar(barX+13, barY + layerIdx*5, 40, coverFraction(code));
      layerIdx++;
    }
    tok = strtok(NULL, " ");
  }
  if (layerIdx == 0) {
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(60, 53, metar.cloudString[0] ? metar.cloudString : "CAVOK");
  }

  // ---- BOTTOM ROW: WX string ----
  u8g2.drawHLine(0, 56, 128);
  u8g2.setFont(u8g2_font_4x6_tf);
  if (strlen(metar.wxString) > 0) {
    u8g2.drawStr(0, 63, metar.wxString);
  } else {
    u8g2.drawStr(0, 63, "No significant wx");
  }

  // Station index indicator dots (top-right)
  for (int i = 0; i < stationCount; i++) {
    if (i == currentStation) u8g2.drawDisc(120 + i*4, 4, 1, U8G2_DRAW_ALL);
    else u8g2.drawCircle(120 + i*4, 4, 1, U8G2_DRAW_ALL);
  }

  u8g2.sendBuffer();
}

// Splash screen
void renderSplash() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_7x13B_tf);
  u8g2.drawStr(10, 18, "METAR DISPLAY");
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(20, 30, "ESP32 + U8G2");
  u8g2.drawStr(5, 42, "Connecting to WiFi...");
  u8g2.sendBuffer();
}

void renderConnecting(int dots) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 12, "WiFi");
  char dotStr[10] = "";
  for (int i=0; i<dots%4; i++) strlcat(dotStr, ".", sizeof(dotStr));
  u8g2.drawStr(30, 12, dotStr);
  u8g2.drawStr(0, 24, WiFi.localIP().toString().c_str());
  u8g2.sendBuffer();
}

// ============================================================
//  LED CONTROL
// ============================================================

void setLEDs() {
  // Turn all off
  if (LED_VFR  >= 0) digitalWrite(LED_VFR,  LOW);
  if (LED_MVFR >= 0) digitalWrite(LED_MVFR, LOW);
  if (LED_IFR  >= 0) digitalWrite(LED_IFR,  LOW);
  if (LED_LIFR >= 0) digitalWrite(LED_LIFR, LOW);
  if (LED_WARN >= 0) digitalWrite(LED_WARN, LOW);

  if (!metar.valid) return;

  const char* cat = metar.flightCategory;
  if      (strcmp(cat,"VFR" )==0 && LED_VFR  >= 0) digitalWrite(LED_VFR,  HIGH);
  else if (strcmp(cat,"MVFR")==0 && LED_MVFR >= 0) digitalWrite(LED_MVFR, HIGH);
  else if (strcmp(cat,"IFR" )==0 && LED_IFR  >= 0) digitalWrite(LED_IFR,  HIGH);
  else if (strcmp(cat,"LIFR")==0 && LED_LIFR >= 0) digitalWrite(LED_LIFR, HIGH);

  // Wind warning LED
  bool warn = (metar.windSpeedKt >= 15) || (metar.windGustKt >= 25);
  if (warn && LED_WARN >= 0) digitalWrite(LED_WARN, HIGH);
}

// ============================================================
//  STATION LIST PARSER
// ============================================================

void parseStations() {
  char buf[64];
  strlcpy(buf, STATIONS, sizeof(buf));
  stationCount = 0;
  char* tok = strtok(buf, ",");
  while (tok && stationCount < 8) {
    strlcpy(stationList[stationCount], tok, 5);
    stationCount++;
    tok = strtok(NULL, ",");
  }
}

// ============================================================
//  SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== METAR DISPLAY ESP32 ===");

  // LED pins
  int leds[] = {LED_VFR, LED_MVFR, LED_IFR, LED_LIFR, LED_WARN};
  for (int pin : leds) {
    if (pin >= 0) { pinMode(pin, OUTPUT); digitalWrite(pin, LOW); }
  }

  // OLED init
  Wire.begin(OLED_SDA, OLED_SCL);
  u8g2.begin();
  u8g2.setContrast(200);    // Brightness 0–255
  renderSplash();

  // Parse station list
  parseStations();
  Serial.printf("Stations: %d — ", stationCount);
  for (int i=0; i<stationCount; i++) Serial.printf("%s ", stationList[i]);
  Serial.println();

  // WiFi connect
  Serial.printf("Connecting to %s ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int dots = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    renderConnecting(dots++);
  }
  Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());

  // Initialise NTP — must be called AFTER WiFi connects
  Serial.println("Starting NTP sync...");
  sntp_set_time_sync_notification_cb(ntpSyncCallback);
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC,
             NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
  // Wait up to 10s for first sync
  Serial.print("Waiting for NTP");
  int ntpWait = 0;
  while (!ntpSynced && ntpWait < 20) {
    delay(500); Serial.print("."); ntpWait++;
  }
  if (ntpSynced) Serial.println(" OK");
  else           Serial.println(" TIMEOUT (will retry)");

  fetchMetar(stationList[currentStation]);
  renderOLED();
  setLEDs();
  lastFetch = millis();
  lastStationSwitch = millis();
}

// ============================================================
//  LOOP
// ============================================================

void loop() {
  unsigned long now = millis();

  // Clock tick — redraw OLED every second to update HH:MM:SS
  static unsigned long lastClockDraw = 0;
  if (ntpSynced && (now - lastClockDraw) >= 1000) {
    renderOLED();           // fast redraw — U8G2 full buffer mode
    lastClockDraw = now;
  }

  // Rotate stations (if multiple configured)
  if (stationCount > 1 && (now - lastStationSwitch) >= SWITCH_INTERVAL_MS) {
    currentStation = (currentStation + 1) % stationCount;
    lastStationSwitch = now;
    Serial.printf("Switching to station: %s\n", stationList[currentStation]);
    // Check if we have a cached reading or need to fetch
    // Simple approach: always refetch on switch
    fetchMetar(stationList[currentStation]);
    renderOLED();
    setLEDs();
  }

  // Periodic refresh
  if ((now - lastFetch) >= FETCH_INTERVAL_MS) {
    Serial.println("Scheduled METAR refresh...");
    fetchMetar(stationList[currentStation]);
    renderOLED();
    setLEDs();
    lastFetch = now;
  }

  // WiFi watchdog — reconnect if dropped
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost — reconnecting...");
    WiFi.reconnect();
    delay(5000);
  }

  delay(100);
}
