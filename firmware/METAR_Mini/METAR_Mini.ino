
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <SPI.h>
#include <time.h>
#include <math.h>

// =====================================================
// METAR WEATHER STATION v4.5
// ESP32-2432S028 / Cheap Yellow Display
//
// NEW IN v4.5
// - Darker VFR category green (#008F32)
// - W13/KW13 alternate runway identifier matching
// - Per-airport AUTO, FILTER, and MANUAL runway configuration
// - Manual runways accept XX/YY, e.g. 06/24 (approximate headings)
// - Up to 20 airports
// - Reliable Wi-Fi configuration with manual SSID
// - Refresh Wi-Fi scan from phone
// - Rotation field shows saved/current value
// - Batched NOAA METAR requests
//
// Preserved from v4.3
// - Bold fonts and existing screen layout
// - Automatic worldwide runway retrieval
// - Maximum-headwind runway highlighted green
// - Touch top to advance airport
// - Touch bottom to enter setup
// - Touch top in setup to exit
// - Time zones, temperature units, clocks
// - Existing saved preferences and runway cache
// =====================================================

// VERIFIED CYD HARDWARE SETTINGS
#define TOUCH_CS 33
#define TOUCH_IRQ 36
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK 25

#define MAX_AIRPORTS 20
#define MAX_RUNWAY_ENDS 16
#define METAR_BATCH_SIZE 5

const unsigned long REFRESH_MS = 300000UL;
const unsigned long RETRY_MS = 30000UL;
const unsigned long WIFI_RETRY_MS = 15000UL;
const unsigned long RUNWAY_RETRY_MS = 3600000UL;

const char* AP_NAME = "METAR-Setup";

const char* RUNWAY_URL =
  "https://davidmegginson.github.io/"
  "ourairports-data/runways.csv";

const uint16_t BG = TFT_BLACK;  // Pure black for clearer MVFR banner contrast
const uint16_t FG = TFT_WHITE;
const uint16_t DIM = TFT_WHITE;  // White headings and secondary text for high contrast
const uint16_t BEST_GREEN = TFT_GREEN;
const uint16_t VFR_GREEN = BEST_GREEN;

TFT_eSPI tft = TFT_eSPI();

SPIClass touchSPI(VSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);

WebServer server(80);
DNSServer dns;
Preferences prefs;

struct RunwayEnd {
  String id;
  float headingTrue;
};

struct AirportRunways {
  RunwayEnd ends[MAX_RUNWAY_ENDS];
  int count = 0;
  bool loaded = false;
};

AirportRunways runwayData[MAX_AIRPORTS];
// Modes: 0=AUTO, 1=FILTER downloaded ends, 2=MANUAL runway ends.
struct RunwayPreference {
  uint8_t mode = 0;
  String spec;
};
RunwayPreference runwayPrefs[MAX_AIRPORTS];

String airports[MAX_AIRPORTS];
String metarCache[MAX_AIRPORTS];

String savedSSID;
String savedPassword;

String scannedSSID[30];
int scannedRSSI[30];
int scannedCount = 0;
bool scanAttempted = false;

int airportCount = 0;

// Configuration is saved per ICAO code, so reordering airports is safe.
String runwayPreferenceKey(String airport) { return "p" + airport; }

void loadRunwayPreferences() {
  prefs.begin("metar-rwy45", true);
  for (int a = 0; a < airportCount; a++) {
    String raw = prefs.getString(runwayPreferenceKey(airports[a]).c_str(), "0|");
    int separator = raw.indexOf('|');
    int mode = raw.substring(0, separator < 0 ? raw.length() : separator).toInt();
    runwayPrefs[a].mode = (mode >= 0 && mode <= 2) ? mode : 0;
    runwayPrefs[a].spec = separator >= 0 ? raw.substring(separator + 1) : "";
  }
  prefs.end();
}

void saveRunwayPreferences() {
  prefs.begin("metar-rwy45", false);
  for (int a = 0; a < airportCount; a++) {
    prefs.putString(runwayPreferenceKey(airports[a]).c_str(),
      String(runwayPrefs[a].mode) + "|" + runwayPrefs[a].spec);
  }
  prefs.end();
}

// FILTER format: 05/23,14/32 (individual ends are also accepted).
// MANUAL format: 06/24,13/31 (runway numbers only).
bool runwayIsSelected(int a, const String &id) {
  if (runwayPrefs[a].mode != 1) return true;
  String tokens = runwayPrefs[a].spec;
  tokens.toUpperCase();
  tokens.replace(" ", "");
  tokens.replace("/", ",");
  tokens += ",";
  int start = 0;
  while (start < tokens.length()) {
    int end = tokens.indexOf(',', start);
    if (end < 0) break;
    if (tokens.substring(start, end) == id) return true;
    start = end + 1;
  }
  return false;
}



int currentAirport = 0;
int rotationSeconds = 5;

bool useFahrenheit = true;
bool use24Hour = false;
bool observationUTC = false;

String timezoneName = "America/New_York";
String timezoneRule =
  "EST5EDT,M3.2.0,M11.1.0";

bool setupMode = false;
bool portalInitialized = false;
bool lastTouchDown = false;
bool firstFetch = true;

unsigned long lastRotate = 0;
unsigned long lastFetch = 0;
unsigned long lastWiFiRetry = 0;
unsigned long lastRunwayAttempt = 0;
unsigned long restartAt = 0;

long lastClockMinute = -1;

// =====================================================
// TIME ZONE PRESETS
// =====================================================

struct TimeZonePreset {
  const char* name;
  const char* rule;
};

const TimeZonePreset zones[] = {
  {"UTC", "UTC0"},
  {"America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
  {"America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
  {"America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
  {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
  {"America/Phoenix", "MST7"},
  {"America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0"},
  {"Pacific/Honolulu", "HST10"},
  {"America/Toronto", "EST5EDT,M3.2.0,M11.1.0"},
  {"America/Vancouver", "PST8PDT,M3.2.0,M11.1.0"},
  {"America/Mexico_City", "CST6"},
  {"America/Bogota", "COT5"},
  {"America/Lima", "PET5"},
  {"America/Sao_Paulo", "BRT3"},
  {"America/Argentina/Buenos_Aires", "ART3"},
  {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0/2"},
  {"Europe/Dublin", "GMT0IST,M3.5.0/1,M10.5.0/2"},
  {"Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
  {"Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
  {"Europe/Istanbul", "TRT-3"},
  {"Africa/Johannesburg", "SAST-2"},
  {"Africa/Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24"},
  {"Africa/Nairobi", "EAT-3"},
  {"Asia/Dubai", "GST-4"},
  {"Asia/Kolkata", "IST-5:30"},
  {"Asia/Kathmandu", "NPT-5:45"},
  {"Asia/Dhaka", "BST-6"},
  {"Asia/Bangkok", "ICT-7"},
  {"Asia/Singapore", "SGT-8"},
  {"Asia/Hong_Kong", "HKT-8"},
  {"Asia/Shanghai", "CST-8"},
  {"Asia/Tokyo", "JST-9"},
  {"Asia/Seoul", "KST-9"},
  {"Australia/Perth", "AWST-8"},
  {"Australia/Darwin", "ACST-9:30"},
  {"Australia/Brisbane", "AEST-10"},
  {"Australia/Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
  {"Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
  {"Australia/Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
  {"Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
  {"Pacific/Fiji", "FJT-12"}
};

const int zoneCount = sizeof(zones) / sizeof(zones[0]);

// =====================================================
// HELPERS
// =====================================================

String htmlEscape(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  s.replace("'", "&#39;");
  return s;
}

String valueText(JsonVariantConst v, String fallback = "--") {
  if (v.isNull()) return fallback;
  return v.as<String>();
}

void applyTimezone() {
  setenv("TZ", timezoneRule.c_str(), 1);
  tzset();
}

// Bold lettering, same font sizes as v4.3
void drawHeavyText(
  String text,
  int x,
  int y,
  int font,
  uint16_t color,
  uint16_t background,
  uint8_t datum
) {
  tft.setTextDatum(datum);

  tft.setTextColor(color, background);
  tft.drawString(text, x, y, font);

  tft.setTextColor(color);
  tft.drawString(text, x + 1, y, font);
}

void statusMessage(String title, String detail) {
  tft.fillScreen(BG);

  drawHeavyText(
    "METAR STATION", 120, 40, 4,
    FG, BG, MC_DATUM
  );

  drawHeavyText(
    title, 120, 145, 2,
    FG, BG, MC_DATUM
  );

  drawHeavyText(
    detail, 120, 180, 2,
    FG, BG, MC_DATUM
  );
}

// =====================================================
// AIRPORT CONFIGURATION - NOW 20 AIRPORTS
// =====================================================

void parseAirports(String input) {
  airportCount = 0;

  input.toUpperCase();
  input.replace("\r", ",");
  input.replace("\n", ",");
  input.replace(";", ",");
  input.replace(" ", ",");

  int start = 0;

  while (
    start < input.length() &&
    airportCount < MAX_AIRPORTS
  ) {
    int end = input.indexOf(',', start);
    if (end < 0) end = input.length();

    String id = input.substring(start, end);
    id.trim();

    bool valid = id.length() == 4;

    for (int j = 0; j < id.length(); j++) {
      char c = id[j];
      if (!((c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9'))) {
        valid = false;
      }
    }

    if (valid) {
      bool duplicate = false;

      for (int j = 0; j < airportCount; j++) {
        if (airports[j] == id) duplicate = true;
      }

      if (!duplicate) {
        airports[airportCount++] = id;
      }
    }

    start = end + 1;
  }

  if (airportCount == 0) {
    airports[0] = "KRDU";
    airportCount = 1;
  }

  for (int i = 0; i < MAX_AIRPORTS; i++) {
    metarCache[i] = "";
    runwayData[i].count = 0;
    runwayData[i].loaded = false;
  }

  currentAirport = 0;
}

String airportListText() {
  String result;

  for (int i = 0; i < airportCount; i++) {
    if (i) result += "\n";
    result += airports[i];
  }

  return result;
}

void saveSettings() {
  // Preserve the v3/v4 storage layout
  prefs.begin("metar-v3", false);
  prefs.putString("airports", airportListText());
  prefs.end();

  prefs.begin("metar-v4", false);
  prefs.putInt("rotate", rotationSeconds);
  prefs.putBool("fahrenheit", useFahrenheit);
  prefs.putBool("hour24", use24Hour);
  prefs.putBool("obsUTC", observationUTC);
  prefs.putString("tzname", timezoneName);
  prefs.putString("tzrule", timezoneRule);
  prefs.end();
}

void loadSettings() {
  prefs.begin("metar-v4", true);

  rotationSeconds = prefs.getInt("rotate", 5);
  useFahrenheit = prefs.getBool("fahrenheit", true);
  use24Hour = prefs.getBool("hour24", false);
  observationUTC = prefs.getBool("obsUTC", false);

  timezoneName = prefs.getString(
    "tzname", "America/New_York"
  );

  timezoneRule = prefs.getString(
    "tzrule", "EST5EDT,M3.2.0,M11.1.0"
  );

  prefs.end();

  rotationSeconds = constrain(rotationSeconds, 3, 30);

  if (timezoneRule.length() == 0) {
    timezoneRule = "UTC0";
    timezoneName = "UTC";
  }

  applyTimezone();
}

// =====================================================
// RUNWAY CACHE
// =====================================================

void addRunway(int a, String id, float heading) {
  if (a < 0 || a >= airportCount) return;

  if (
    id.length() == 0 ||
    id == "NA" ||
    heading < 0 ||
    heading > 360
  ) return;

  AirportRunways &r = runwayData[a];

  if (r.count >= MAX_RUNWAY_ENDS) return;

  for (int i = 0; i < r.count; i++) {
    if (r.ends[i].id == id) return;
  }

  r.ends[r.count].id = id;
  r.ends[r.count].headingTrue = heading;
  r.count++;
}

String runwayCacheKey(String airport) {
  return "r" + airport;
}

void saveRunwayCache() {
  prefs.begin("metar-rwy", false);

  for (int a = 0; a < airportCount; a++) {
    if (!runwayData[a].loaded || runwayPrefs[a].mode == 2) continue;

    String data;

    for (int i = 0; i < runwayData[a].count; i++) {
      if (i) data += ",";
      data += runwayData[a].ends[i].id;
      data += ":";
      data += String(runwayData[a].ends[i].headingTrue, 1);
    }

    prefs.putString(
      runwayCacheKey(airports[a]).c_str(), data
    );
  }

  prefs.end();
}

void loadRunwayCache() {
  prefs.begin("metar-rwy", true);

  for (int a = 0; a < airportCount; a++) {
    String data = prefs.getString(
      runwayCacheKey(airports[a]).c_str(), ""
    );

    if (data.length() == 0) continue;

    int start = 0;

    while (start < data.length()) {
      int end = data.indexOf(',', start);
      if (end < 0) end = data.length();

      String entry = data.substring(start, end);
      int colon = entry.indexOf(':');

      if (colon > 0) {
        String id = entry.substring(0, colon);
        float heading =
          entry.substring(colon + 1).toFloat();

        addRunway(a, id, heading);
      }

      start = end + 1;
    }

    runwayData[a].loaded = runwayData[a].count > 0;
  }

  prefs.end();
}

// MANUAL: enter runway designators, e.g. 06/24, 13/31.
// Runway numbers are approximate MAGNETIC headings. For the METAR
// true-wind comparison, we use the number x 10 as an approximation.
// Without local magnetic variation, headwind selection is approximate.
// Optional legacy 06:060 format remains accepted for saved settings.
void addManualRunwayEnd(int a, String token) {
  token.trim();
  token.toUpperCase();
  if (token.length() < 2) return;

  int colon = token.indexOf(':');
  String id = colon >= 0 ? token.substring(0, colon) : token;
  id.trim();
  if (id.length() < 2 || id.length() > 3) return;
  if (!isDigit(id[0]) || !isDigit(id[1])) return;
  int number = (id[0] - '0') * 10 + (id[1] - '0');
  if (number < 1 || number > 36) return;
  if (id.length() == 3 && id[2] != 'L' && id[2] != 'R' && id[2] != 'C') return;

  float heading = (number == 36) ? 360.0f : number * 10.0f;
  if (colon >= 0) {
    String supplied = token.substring(colon + 1);
    supplied.trim();
    if (supplied.length() == 0) return;
    for (int i = 0; i < supplied.length(); i++) {
      char c = supplied[i];
      if (!isDigit(c) && c != '.') return;
    }
    heading = supplied.toFloat();
  }
  addRunway(a, id, heading);
}

void applyManualRunways() {
  for (int a = 0; a < airportCount; a++) {
    if (runwayPrefs[a].mode != 2) continue;
    runwayData[a].count = 0;
    runwayData[a].loaded = false;
    String entries = runwayPrefs[a].spec;
    entries.replace("/", ",");
    entries.replace(";", ",");
    entries += ",";
    int start = 0;
    while (start < entries.length()) {
      int end = entries.indexOf(',', start);
      if (end < 0) break;
      addManualRunwayEnd(a, entries.substring(start, end));
      start = end + 1;
    }
    runwayData[a].loaded = runwayData[a].count > 0;
  }
}

// =====================================================
// RUNWAY CSV PARSER
// =====================================================

String csvField(const String &line, int wanted) {
  int field = 0;
  bool quoted = false;
  String result;

  for (int i = 0; i <= line.length(); i++) {
    char c = i < line.length() ? line[i] : ',';

    if (c == '"') {
      if (
        quoted &&
        i + 1 < line.length() &&
        line[i + 1] == '"'
      ) {
        if (field == wanted) result += '"';
        i++;
      } else {
        quoted = !quoted;
      }
    } else if (c == ',' && !quoted) {
      if (field == wanted) return result;
      field++;
      result = "";
    } else if (field == wanted) {
      result += c;
    }
  }

  return "";
}

void processRunwayCSVLine(const String &line) {
  String airport = csvField(line, 2);
  int index = -1;

  for (int i = 0; i < airportCount; i++) {
    // Some US local airports use W13 in OurAirports but KW13 in METARs.
    // Restrict K-prefix fallback to a three-character US local identifier.
    bool localAlias = airport.length() == 3 && airports[i].length() == 4 && airports[i][0] == 'K' &&
      airports[i].substring(1) == airport &&
      (airport[0] >= '0' && airport[0] <= '9' ||
       airport[1] >= '0' && airport[1] <= '9' ||
       airport[2] >= '0' && airport[2] <= '9');
    if (airports[i] == airport || localAlias) {
      index = i;
      break;
    }
  }

  if (index < 0) return;
  if (runwayPrefs[index].mode == 2) return;
  if (csvField(line, 7) == "1") return;

  String lowID = csvField(line, 8);
  String lowHeading = csvField(line, 12);

  String highID = csvField(line, 14);
  String highHeading = csvField(line, 18);

  if (lowHeading.length() > 0) {
    addRunway(index, lowID, lowHeading.toFloat());
  }

  if (highHeading.length() > 0) {
    addRunway(index, highID, highHeading.toFloat());
  }
}

// =====================================================
// RUNWAY DOWNLOAD
// Stream CSV without loading it into RAM.
// Cache is retained when a download fails.
// =====================================================

bool downloadRunwayDatabase() {
  if (WiFi.status() != WL_CONNECTED) return false;

  Serial.println("Downloading runway database...");

  statusMessage("RUNWAY DATABASE", "Downloading...");

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, RUNWAY_URL)) return false;

  http.setTimeout(15000);

  int code = http.GET();
  Serial.printf("Runway HTTP: %d\n", code);

  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  // Do not clear existing runway cache.
  // Only airports with missing runway data need records.
  bool missing[MAX_AIRPORTS];

  for (int i = 0; i < airportCount; i++) {
    missing[i] = !runwayData[i].loaded && runwayPrefs[i].mode != 2;
  }

  WiFiClient *stream = http.getStreamPtr();

  int remaining = http.getSize();
  bool knownLength = remaining >= 0;

  String line;
  line.reserve(512);

  unsigned long lastByte = millis();
  unsigned long totalBytes = 0;
  bool complete = false;

  while (true) {
    if (stream->available() > 0) {
      int c = stream->read();
      if (c < 0) continue;

      lastByte = millis();
      totalBytes++;

      if (knownLength) remaining--;

      if (c == '\n') {
        if (line.length() > 0) {
          String airport = csvField(line, 2);

          for (int a = 0; a < airportCount; a++) {
            if (missing[a] && (airports[a] == airport ||
              (airport.length() == 3 && airports[a].length() == 4 &&
               airports[a][0] == 'K' && airports[a].substring(1) == airport))) {
              processRunwayCSVLine(line);
              break;
            }
          }
        }
        line = "";
      } else if (c != '\r') {
        if (line.length() < 1000) {
          line += (char)c;
        }
      }

      if (knownLength && remaining <= 0) {
        complete = true;
        break;
      }
    } else {
      if (knownLength && remaining <= 0) {
        complete = true;
        break;
      }

      if (!stream->connected()) {
        if (!knownLength) complete = true;
        break;
      }

      if (millis() - lastByte > 15000UL) {
        Serial.println("Runway download timeout");
        break;
      }

      delay(1);
    }

    yield();
  }

  if (complete && line.length() > 0) {
    processRunwayCSVLine(line);
  }

  http.end();

  Serial.printf(
    "Runway bytes received: %lu\n", totalBytes
  );

  if (!complete) {
    Serial.println("Runway download incomplete");
    return false;
  }

  for (int i = 0; i < airportCount; i++) {
    runwayData[i].loaded =
      runwayData[i].count > 0;

    Serial.printf(
      "%s: %d runway ends\n",
      airports[i].c_str(),
      runwayData[i].count
    );
  }

  saveRunwayCache();
  applyManualRunways();
  return true;
}

// =====================================================
// MAXIMUM HEADWIND CALCULATION
// METAR wind and database headings are true.
// =====================================================

int bestRunwayIndex(int a, JsonObjectConst metar) {
  AirportRunways &r = runwayData[a];

  if (r.count == 0) return -1;

  if (
    metar["wdir"].isNull() ||
    metar["wspd"].isNull()
  ) return -1;

  String directionText =
    valueText(metar["wdir"], "");

  if (
    directionText == "VRB" ||
    directionText == "" ||
    directionText == "--"
  ) return -1;

  float windSpeed = metar["wspd"].as<float>();
  if (windSpeed < 1.0f) return -1;

  float windDirection = directionText.toFloat();

  if (windDirection < 0 || windDirection > 360)
    return -1;

  int best = -1;
  float maximumHeadwind = -100000.0f;

  for (int i = 0; i < r.count; i++) {
    if (!runwayIsSelected(a, r.ends[i].id)) continue;
    float difference =
      (windDirection - r.ends[i].headingTrue) *
      DEG_TO_RAD;

    float headwind = windSpeed * cosf(difference);

    if (headwind > maximumHeadwind) {
      maximumHeadwind = headwind;
      best = i;
    }
  }

  if (maximumHeadwind <= 0.0f) return -1;

  return best;
}

// =====================================================
// WEATHER DISPLAY
// =====================================================

String fitText(String text, int font, int maxWidth) {
  if (tft.textWidth(text, font) <= maxWidth)
    return text;

  while (
    text.length() > 0 &&
    tft.textWidth(text + ".", font) > maxWidth
  ) {
    text.remove(text.length() - 1);
  }

  return text + ".";
}

void displayRow(
  int y,
  String label,
  String value,
  uint16_t valueColor = FG
) {
  const int left = 7;
  const int right = 232;

  int labelWidth = tft.textWidth(label, 2);
  int availableWidth =
    right - left - labelWidth - 12;

  if (availableWidth < 40)
    availableWidth = 40;

  value = fitText(value, 2, availableWidth);

  drawHeavyText(
    label, left, y, 2,
    DIM, BG, TL_DATUM
  );

  drawHeavyText(
    value, right - 1, y, 2,
    valueColor, BG, TR_DATUM
  );
}

void drawRunways(JsonObjectConst metar) {
  const int y = 235;

  drawHeavyText(
    "RUNWAYS", 7, y, 2,
    DIM, BG, TL_DATUM
  );

  AirportRunways &r = runwayData[currentAirport];

  bool hasVisibleRunways = false;
  for (int i = 0; i < r.count; i++)
    if (runwayIsSelected(currentAirport, r.ends[i].id)) hasVisibleRunways = true;

  if (!hasVisibleRunways) {
    drawHeavyText(
      "Unavailable", 232, y, 2,
      DIM, BG, TR_DATUM
    );
    return;
  }

  int best = bestRunwayIndex(
    currentAirport, metar
  );

  int order[MAX_RUNWAY_ENDS];
  int n = 0;

  if (best >= 0) order[n++] = best;

  for (int i = 0; i < r.count; i++) {
    if (i != best && runwayIsSelected(currentAirport, r.ends[i].id)) order[n++] = i;
  }

  int x = 232;
  int minimumX = 91;

  for (int j = n - 1; j >= 0; j--) {
    int i = order[j];

    String id = r.ends[i].id;
    int width = tft.textWidth(id, 2) + 7;

    if (x - width < minimumX) continue;

    uint16_t color =
      (i == best) ? BEST_GREEN : FG;

    drawHeavyText(
      id, x - 1, y, 2,
      color, BG, TR_DATUM
    );

    x -= width;
  }
}

// =====================================================
// WEATHER FORMATTERS
// =====================================================

String temperatureText(JsonVariantConst v) {
  if (v.isNull()) return "--";

  float c = v.as<float>();

  if (useFahrenheit)
    c = c * 1.8f + 32.0f;

  return String((int)roundf(c));
}

String cloudDescription(JsonVariantConst v) {
  JsonArrayConst clouds = v.as<JsonArrayConst>();

  if (clouds.isNull() || clouds.size() == 0)
    return "None reported";

  String result;
  int shown = 0;

  for (JsonObjectConst cloud : clouds) {
    String cover = valueText(cloud["cover"], "");

    if (cover.length() == 0) continue;

    if (shown > 0) result += " ";
    result += cover;

    if (!cloud["base"].isNull()) {
      char buffer[12];

      snprintf(
        buffer, sizeof(buffer), "%03d",
        cloud["base"].as<int>() / 100
      );

      result += buffer;
    }

    if (++shown >= 2) break;
  }

  return result.length() ?
    result : "None reported";
}

String weatherDescription(String wx) {
  wx.trim();

  if (wx.length() == 0 || wx == "--")
    return "None reported";

  if (wx == "-RA") return "Light rain";
  if (wx == "RA") return "Rain";
  if (wx == "+RA") return "Heavy rain";
  if (wx == "-SN") return "Light snow";
  if (wx == "SN") return "Snow";
  if (wx == "+SN") return "Heavy snow";
  if (wx == "TS") return "Thunderstorm";
  if (wx == "TSRA") return "Thunderstorm/rain";
  if (wx == "-TSRA") return "Light TS rain";
  if (wx == "+TSRA") return "Heavy TS rain";
  if (wx == "BR") return "Mist";
  if (wx == "FG") return "Fog";
  if (wx == "HZ") return "Haze";
  if (wx == "FU") return "Smoke";
  if (wx == "FZRA") return "Freezing rain";
  if (wx == "-DZ") return "Light drizzle";
  if (wx == "DZ") return "Drizzle";
  if (wx == "SHRA") return "Rain showers";
  if (wx == "-SHRA") return "Light showers";

  return wx;
}

// =====================================================
// CLOCKS
// =====================================================

String formatTime(time_t timestamp, bool utc) {
  if (timestamp < 1700000000)
    return "--:--";

  struct tm tmValue;

  if (utc)
    gmtime_r(&timestamp, &tmValue);
  else
    localtime_r(&timestamp, &tmValue);

  char buffer[32];

  strftime(
    buffer, sizeof(buffer),
    use24Hour ? "%H:%M" : "%I:%M%p",
    &tmValue
  );

  String result(buffer);

  if (!use24Hour && result.startsWith("0"))
    result.remove(0, 1);

  return result;
}

void drawClockLine(long obsTimestamp) {
  tft.fillRect(0, 277, 240, 43, BG);

  time_t now = time(nullptr);

  String localClock = formatTime(now, false);

  String obsClock = formatTime(
    (time_t)obsTimestamp,
    observationUTC
  );

  String left = "LOCAL " + localClock;

  String right =
    (observationUTC ? "Z " : "OBS ") +
    obsClock;

  drawHeavyText(
    left, 5, 283, 2,
    FG, BG, TL_DATUM
  );

  drawHeavyText(
    right, 234, 283, 2,
    FG, BG, TR_DATUM
  );

  if (now >= 1700000000 && obsTimestamp > 0) {
    long ageMinutes =
      (long)(now - obsTimestamp) / 60;

    if (ageMinutes >= 60) {
      drawHeavyText(
        ageMinutes >= 120 ?
          "STALE METAR" : "OLD METAR",
        120, 311, 2,
        TFT_YELLOW, BG, MC_DATUM
      );
    }
  }
}

// =====================================================
// MAIN METAR SCREEN
// =====================================================

uint16_t categoryColor(String category) {
  if (category == "VFR") return VFR_GREEN;
  if (category == "MVFR") return TFT_BLUE;
  if (category == "IFR") return TFT_RED;
  if (category == "LIFR") return TFT_MAGENTA;

  return TFT_DARKGREY;
}

void drawMETAR(JsonObjectConst metar) {
  String airport = valueText(
    metar["icaoId"],
    airports[currentAirport]
  );

  String category =
    valueText(metar["fltCat"], "UNKNOWN");

  tft.fillScreen(BG);

  uint16_t banner = categoryColor(category);
  tft.fillRect(0, 0, 240, 46, banner);

  drawHeavyText(
    airport, 10, 23, 4,
    TFT_WHITE, banner, ML_DATUM
  );

  int categoryFont = 4;

  if (
    tft.textWidth(airport, 4) +
    tft.textWidth(category, 4) + 24 > 240
  ) {
    categoryFont = 2;
  }

  drawHeavyText(
    category, 230, 23, categoryFont,
    TFT_WHITE, banner, MR_DATUM
  );

  // WIND
  String wind = "--";

  String direction = valueText(metar["wdir"]);
  String speed = valueText(metar["wspd"]);
  String gust = valueText(metar["wgst"], "");

  if (speed != "--") {
    if (speed == "0" && direction == "0") {
      wind = "CALM";
    } else {
      wind =
        (direction == "--" ? "VRB" : direction) +
        " @ " + speed + " kt";

      if (gust.length() > 0 && gust != "0")
        wind += " G" + gust;
    }
  }

  displayRow(55, "WIND", wind);

  // CLOUDS
  displayRow(
    85, "CLOUDS",
    cloudDescription(metar["clouds"])
  );

  // VISIBILITY
  String visibility = valueText(metar["visib"]);

  if (visibility != "--")
    visibility += " SM";

  displayRow(115, "VIS", visibility);

  // ALTIMETER
  String altimeter = "--";

  if (!metar["altim"].isNull()) {
    altimeter = String(
      metar["altim"].as<float>() *
      0.029529983f,
      2
    );
  }

  displayRow(145, "ALT", altimeter + " inHg");

  // TEMPERATURE / DEW POINT
  String tempDew =
    temperatureText(metar["temp"]) +
    " / " +
    temperatureText(metar["dewp"]) +
    (useFahrenheit ? " F" : " C");

  displayRow(175, "TEMP/DEW", tempDew);

  // WEATHER
  String weather = weatherDescription(
    valueText(metar["wxString"], "")
  );

  displayRow(
    205, "WEATHER", weather,
    weather == "None reported" ?
      DIM : TFT_YELLOW
  );

  // RUNWAYS
  drawRunways(metar);

  // CLOCKS
  tft.drawFastHLine(
    5, 272, 230, TFT_DARKGREY
  );

  long obsTimestamp =
    metar["obsTime"] | 0L;

  drawClockLine(obsTimestamp);
}

void drawCurrentAirport() {
  if (airportCount == 0) return;

  if (metarCache[currentAirport].length() == 0) {
    statusMessage(
      airports[currentAirport],
      "No METAR available"
    );
    return;
  }

  JsonDocument doc;

  if (deserializeJson(
    doc, metarCache[currentAirport]
  )) {
    statusMessage(
      airports[currentAirport],
      "METAR data error"
    );
    return;
  }

  drawMETAR(doc.as<JsonObjectConst>());
}

// =====================================================
// NOAA METAR RETRIEVAL
// NEW: Fetch airports in batches of 5.
// =====================================================

bool fetchAllMETARs() {
  if (WiFi.status() != WL_CONNECTED)
    return false;

  bool anySuccess = false;
  bool allSuccess = true;

  for (
    int start = 0;
    start < airportCount;
    start += METAR_BATCH_SIZE
  ) {
    String ids;

    int end = min(
      start + METAR_BATCH_SIZE,
      airportCount
    );

    for (int i = start; i < end; i++) {
      if (ids.length()) ids += ",";
      ids += airports[i];
    }

    String url =
      "https://aviationweather.gov/api/data/metar"
      "?ids=" + ids + "&format=json";

    Serial.println("Fetching: " + ids);

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;

    if (!http.begin(client, url)) {
      allSuccess = false;
      continue;
    }

    http.setTimeout(12000);

    http.addHeader(
      "User-Agent",
      "ESP32-METAR-Display/4.4"
    );

    int code = http.GET();

    Serial.printf("METAR HTTP: %d\n", code);

    if (code != HTTP_CODE_OK) {
      allSuccess = false;
      http.end();
      continue;
    }

    // Read only this batch's response.
    String payload = http.getString();
    http.end();

    JsonDocument doc;

    if (deserializeJson(doc, payload)) {
      allSuccess = false;
      continue;
    }

    JsonArrayConst reports =
      doc.as<JsonArrayConst>();

    if (reports.isNull()) {
      allSuccess = false;
      continue;
    }

    anySuccess = true;

    // Preserve the previous report for an airport
    // when NOAA has no new observation for it.
    for (JsonObjectConst report : reports) {
      String id = valueText(
        report["icaoId"], ""
      );

      for (int i = start; i < end; i++) {
        if (airports[i] == id) {
          String reportJSON;
          serializeJson(report, reportJSON);
          metarCache[i] = reportJSON;
          break;
        }
      }
    }

    delay(20);
  }

  drawCurrentAirport();

  return anySuccess && allSuccess;
}

// =====================================================
// WI-FI SCANNING
//
// NEW:
// - Scan while setup AP remains active
// - Refresh on demand
// - Manual SSID is always available
// - No dependency on old network being connected
// =====================================================

void scanAvailableNetworks() {
  Serial.println("Scanning nearby Wi-Fi networks...");

  scannedCount = 0;
  scanAttempted = true;

  // Ensure both AP and station interfaces are enabled.
  // Keep the setup AP running for the phone.
  WiFi.mode(WIFI_AP_STA);

  // Disconnect the old station connection without
  // erasing credentials stored in Preferences.
  WiFi.disconnect(false, false);

  delay(150);

  WiFi.scanDelete();

  int found = WiFi.scanNetworks(
    false,  // synchronous
    true    // include hidden networks
  );

  Serial.printf("Wi-Fi networks found: %d\n", found);

  if (found > 0) {
    for (int i = 0; i < found; i++) {
      String ssid = WiFi.SSID(i);

      // Hidden networks without a broadcast name
      // can be entered using the manual SSID field.
      if (ssid.length() == 0) continue;

      bool duplicate = false;

      for (int j = 0; j < scannedCount; j++) {
        if (scannedSSID[j] == ssid) {
          duplicate = true;
          break;
        }
      }

      if (duplicate) continue;

      if (scannedCount < 30) {
        scannedSSID[scannedCount] = ssid;
        scannedRSSI[scannedCount] = WiFi.RSSI(i);
        scannedCount++;
      }
    }
  }

  WiFi.scanDelete();

  Serial.printf(
    "Unique visible networks: %d\n",
    scannedCount
  );
}

// =====================================================
// PHONE SETUP PAGE
//
// Build HTML using String concatenation rather than
// embedding dynamic values inside raw string fragments.
// This ensures the rotation value appears reliably.
// =====================================================

String htmlPage() {
  String page;
  page.reserve(13000);

  page += R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport"
 content="width=device-width,initial-scale=1">
<title>METAR Station Setup</title>
<style>
body {
 background:#101c3a;
 color:white;
 font-family:Arial,sans-serif;
 padding:18px;
}
main {max-width:460px;margin:auto;}
h1 {color:#79dcff;}
h2 {margin-top:28px;color:#a9dcff;}
label {
 display:block;
 margin-top:18px;
 font-weight:bold;
}
input,textarea,select,button {
 box-sizing:border-box;
 width:100%;
 padding:12px;
 margin-top:7px;
 font-size:16px;
 border-radius:7px;
}
textarea {height:230px;}
button {
 background:#168b46;
 color:white;
 border:0;
 margin-top:20px;
 font-weight:bold;
}
.secondary {background:#53627e;}
small {color:#c4d0e2;}
.info {color:#9bd4f5;}
</style>
</head>
<body>
<main>
<h1>METAR Weather Station</h1>
<p>Version 4.5 configuration</p>

<form action="/save" method="POST">

<h2>Airports</h2>
<label>ICAO airport identifiers</label>
<textarea name="airports" required>
)HTML";

  page += htmlEscape(airportListText());

  page += R"HTML(
</textarea>
<small>Up to 20 airports, one per line.</small>
)HTML";

  page += "<h2>Runway configuration</h2>";
  page += "<small>AUTO is the default. FILTER: 05/23,14/32. ";
  page += "MANUAL: 06/24,13/31 (runway numbers only). ";
  page += "Settings below apply to currently saved airports.</small>";
  for (int a = 0; a < airportCount; a++) {
    page += "<label>" + htmlEscape(airports[a]) + " runway mode</label>";
    page += "<select name='rmode_" + airports[a] + "'>";
    const char* labels[] = {"AUTO", "FILTER", "MANUAL"};
    for (int m = 0; m < 3; m++) {
      page += "<option value='" + String(m) + "'";
      if (runwayPrefs[a].mode == m) page += " selected";
      page += ">" + String(labels[m]) + "</option>";
    }
    page += "</select>";
    page += "<input name='rspec_" + airports[a] + "' placeholder='Format XX/YY (e.g. 06/24)' value='";
    page += htmlEscape(runwayPrefs[a].spec);
    page += "'>";
  }

  page += R"HTML(
<h2>Display</h2>
<label>Seconds per airport (3-30)</label>
)HTML";

  // Explicitly insert the active rotation value.
  page += "<input type=\"number\" name=\"rotate\" ";
  page += "min=\"3\" max=\"30\" step=\"1\" value=\"";
  page += String(rotationSeconds);
  page += "\">";

  page += R"HTML(
<label>Temperature units</label>
<select name="temp">
)HTML";

  page += "<option value='F'";
  if (useFahrenheit) page += " selected";
  page += ">Fahrenheit (F)</option>";

  page += "<option value='C'";
  if (!useFahrenheit) page += " selected";
  page += ">Celsius (C)</option>";

  page += R"HTML(
</select>

<h2>Time</h2>
<label>Time zone</label>
<select name="timezone">
)HTML";

  bool customSelected = true;

  for (int i = 0; i < zoneCount; i++) {
    if (timezoneName == zones[i].name)
      customSelected = false;
  }

  for (int i = 0; i < zoneCount; i++) {
    page += "<option value=\"";
    page += htmlEscape(String(zones[i].name));
    page += "\"";

    if (timezoneName == zones[i].name)
      page += " selected";

    page += ">";
    page += htmlEscape(String(zones[i].name));
    page += "</option>";
  }

  page += "<option value=\"CUSTOM\"";

  if (customSelected)
    page += " selected";

  page += ">Custom POSIX rule</option>";
  page += "</select>";

  page += "<label>Custom POSIX time-zone rule</label>";
  page += "<input type=\"text\" name=\"customtz\" value=\"";
  page += htmlEscape(timezoneRule);
  page += "\">";

  page += R"HTML(
<small>Used only when Custom is selected.</small>

<label>Clock format</label>
<select name="hourformat">
)HTML";

  page += "<option value='12'";
  if (!use24Hour) page += " selected";
  page += ">12-hour (AM/PM)</option>";

  page += "<option value='24'";
  if (use24Hour) page += " selected";
  page += ">24-hour</option>";

  page += R"HTML(
</select>

<label>METAR observation time</label>
<select name="obstime">
)HTML";

  page += "<option value='LOCAL'";
  if (!observationUTC) page += " selected";
  page += ">Local civil time</option>";

  page += "<option value='UTC'";
  if (observationUTC) page += " selected";
  page += ">UTC / Zulu</option>";

  page += R"HTML(
</select>

<h2>Wi-Fi</h2>
<p class="info">Current saved network:
)HTML";

  page += htmlEscape(
    savedSSID.length() ?
      savedSSID : String("None")
  );

  page += R"HTML(
</p>

<label>Choose an available network</label>
<select name="ssid">
<option value="">Keep current Wi-Fi</option>
)HTML";

  for (int i = 0; i < scannedCount; i++) {
    page += "<option value=\"";
    page += htmlEscape(scannedSSID[i]);
    page += "\">";
    page += htmlEscape(scannedSSID[i]);
    page += " (";
    page += String(scannedRSSI[i]);
    page += " dBm)</option>";
  }

  page += "</select>";

  if (scannedCount == 0) {
    page += R"HTML(
<p><small>No networks detected in the last scan.
You can refresh the scan or enter a network name
manually below.</small></p>
)HTML";
  }

  page += R"HTML(
<label>Or enter network name manually (SSID)</label>
<input type="text" name="manualssid"
 placeholder="Enter Wi-Fi network name">

<small>
A manually entered network takes priority over
the selection above.
</small>

<label>Wi-Fi password</label>
<input type="password" name="password"
 placeholder="Password for new network">

<small>
Leave the network selection and manual SSID blank
to keep your existing Wi-Fi credentials.
</small>

<button type="submit">Save and Restart</button>
</form>

<form action="/scan" method="GET">
<button type="submit" class="secondary">
Refresh Wi-Fi Networks
</button>
</form>

<form action="/exit" method="POST">
<button type="submit" class="secondary">
Return to Weather Display
</button>
</form>

<p><small>
You can also tap the upper portion of the
device screen to leave setup without saving.
</small></p>

<p><small>
Runway data is retrieved automatically and cached.
METAR observations refresh every five minutes.
</small></p>

</main>
</body>
</html>
)HTML";

  return page;
}

// =====================================================
// SETUP SCREEN
// =====================================================

void setupScreen() {
  tft.fillScreen(BG);

  drawHeavyText(
    "SETUP MODE", 120, 40, 4,
    TFT_CYAN, BG, MC_DATUM
  );

  drawHeavyText(
    "Connect your phone", 120, 95, 2,
    FG, BG, MC_DATUM
  );

  drawHeavyText(
    "to Wi-Fi network:", 120, 120, 2,
    FG, BG, MC_DATUM
  );

  drawHeavyText(
    AP_NAME, 120, 153, 4,
    TFT_YELLOW, BG, MC_DATUM
  );

  drawHeavyText(
    "No password needed", 120, 193, 2,
    FG, BG, MC_DATUM
  );

  drawHeavyText(
    "Open browser:", 120, 230, 2,
    FG, BG, MC_DATUM
  );

  drawHeavyText(
    "192.168.4.1", 120, 256, 4,
    FG, BG, MC_DATUM
  );

  drawHeavyText(
    "TAP TOP TO EXIT", 120, 303, 2,
    TFT_GREEN, BG, MC_DATUM
  );
}

void scheduleRestart() {
  restartAt = millis() + 2000UL;
}

// =====================================================
// PHONE PORTAL ROUTES
// =====================================================

void initializePortalRoutes() {
  if (portalInitialized) return;

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", htmlPage());
  });

  // NEW: Re-scan Wi-Fi networks.
  server.on("/scan", HTTP_GET, []() {
    scanAvailableNetworks();

    server.sendHeader("Location", "/");
    server.send(303, "text/plain", "");
  });

  server.on("/save", HTTP_POST, []() {
    String airportInput = server.arg("airports");
    parseAirports(airportInput);
    loadRunwayPreferences();
    for (int a = 0; a < airportCount; a++) {
      String modeName = "rmode_" + airports[a];
      String specName = "rspec_" + airports[a];
      if (server.hasArg(modeName)) {
        int m = server.arg(modeName).toInt();
        if (m >= 0 && m <= 2) runwayPrefs[a].mode = m;
        runwayPrefs[a].spec = server.arg(specName);
        runwayPrefs[a].spec.trim();
        if (runwayPrefs[a].spec.length() > 160)
          runwayPrefs[a].spec = runwayPrefs[a].spec.substring(0, 160);
      }
    }
    saveRunwayPreferences();

    // Keep the current interval if the field
    // is missing or blank.
    String rotateText = server.arg("rotate");
    rotateText.trim();

    if (rotateText.length() > 0) {
      int proposed = rotateText.toInt();

      if (proposed >= 3 && proposed <= 30)
        rotationSeconds = proposed;
    }

    useFahrenheit = server.arg("temp") != "C";
    use24Hour = server.arg("hourformat") == "24";
    observationUTC = server.arg("obstime") == "UTC";

    String selectedZone = server.arg("timezone");

    if (selectedZone == "CUSTOM") {
      String custom = server.arg("customtz");
      custom.trim();

      if (
        custom.length() > 0 &&
        custom.length() < 100
      ) {
        timezoneName = "CUSTOM";
        timezoneRule = custom;
      }
    } else {
      for (int i = 0; i < zoneCount; i++) {
        if (selectedZone == zones[i].name) {
          timezoneName = zones[i].name;
          timezoneRule = zones[i].rule;
          break;
        }
      }
    }

    saveSettings();

    // NEW: Manual SSID takes priority.
    String newSSID = server.arg("manualssid");
    newSSID.trim();

    if (newSSID.length() == 0) {
      newSSID = server.arg("ssid");
    }

    String newPassword = server.arg("password");

    // Only overwrite credentials when a new
    // network was explicitly supplied.
    if (newSSID.length() > 0) {
      prefs.begin("metar-wifi", false);
      prefs.putString("ssid", newSSID);
      prefs.putString("password", newPassword);
      prefs.end();

      Serial.println("New Wi-Fi network saved: " + newSSID);
    } else {
      Serial.println("Existing Wi-Fi credentials retained");
    }

    server.send(
      200, "text/html",
      "<html><body>"
      "<h2>Settings saved!</h2>"
      "<p>Restarting...</p>"
      "</body></html>"
    );

    statusMessage(
      "Settings saved", "Restarting..."
    );

    scheduleRestart();
  });

  server.on("/exit", HTTP_POST, []() {
    server.send(
      200, "text/html",
      "<html><body>"
      "<h2>Restarting...</h2>"
      "</body></html>"
    );

    scheduleRestart();
  });

  server.onNotFound([]() {
    server.sendHeader(
      "Location", "http://192.168.4.1/"
    );

    server.send(302, "text/plain", "");
  });

  portalInitialized = true;
}

void startSetupPortal() {
  if (setupMode) return;

  setupMode = true;

  WiFi.mode(WIFI_AP_STA);

  // Start an open setup network.
  WiFi.softAP(AP_NAME);

  dns.start(53, "*", WiFi.softAPIP());

  initializePortalRoutes();
  server.begin();

  setupScreen();

  // The initial scan occurs when entering setup,
  // not during HTML rendering.
  scanAvailableNetworks();

  Serial.println(
    "Setup portal: http://192.168.4.1"
  );
}

// =====================================================
// SAVED WI-FI CONNECTION
// =====================================================

bool connectSavedWiFi() {
  if (savedSSID.length() == 0)
    return false;

  WiFi.mode(WIFI_STA);

  WiFi.begin(
    savedSSID.c_str(),
    savedPassword.c_str()
  );

  statusMessage(
    "Connecting Wi-Fi", savedSSID
  );

  unsigned long start = millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - start < 20000UL
  ) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  return false;
}

// =====================================================
// TOUCHSCREEN
// =====================================================

void handleTouch() {
  bool down = touch.touched();

  if (down && !lastTouchDown) {
    TS_Point p = touch.getPoint();

    if (p.z > 100) {
      int screenX = constrain(
        map(p.x, 330, 3690, 0, 239),
        0, 239
      );

      int screenY = constrain(
        map(p.y, 260, 3730, 0, 319),
        0, 319
      );

      Serial.printf(
        "Touch: %d,%d\n",
        screenX, screenY
      );

      if (screenY >= 240) {
        startSetupPortal();
      } else {
        currentAirport =
          (currentAirport + 1) % airportCount;

        lastRotate = millis();
        drawCurrentAirport();
      }
    }
  }

  lastTouchDown = down;
}

// =====================================================
// ARDUINO SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  // VERIFIED BACKLIGHT
  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);

  // VERIFIED DISPLAY INITIALIZATION
  // KEEP EXACT ORDER
  tft.init();
  tft.setRotation(0);
  tft.invertDisplay(true);

  // VERIFIED TOUCH HARDWARE
  touchSPI.begin(
    TOUCH_CLK,
    TOUCH_MISO,
    TOUCH_MOSI,
    TOUCH_CS
  );

  touch.begin(touchSPI);
  touch.setRotation(0);

  statusMessage(
    "Starting...", "METAR v4.5"
  );

  // Restore existing airport list
  prefs.begin("metar-v3", true);

  String savedAirports =
    prefs.getString("airports", "KRDU");

  prefs.end();

  parseAirports(savedAirports);
  loadSettings();

  // Restore existing runway cache
  loadRunwayPreferences();
  loadRunwayCache();
  applyManualRunways();

  // Restore Wi-Fi credentials
  prefs.begin("metar-wifi", true);

  savedSSID = prefs.getString("ssid", "");
  savedPassword = prefs.getString("password", "");

  prefs.end();

  if (!connectSavedWiFi()) {
    startSetupPortal();
    return;
  }

  configTime(
    0, 0,
    "pool.ntp.org",
    "time.nist.gov"
  );

  applyTimezone();

  firstFetch = true;
  lastRotate = millis();

  if (fetchAllMETARs()) {
    firstFetch = false;
    lastFetch = millis();
  } else {
    Serial.println("Initial METAR download failed");
  }

  // Download runway data only if an airport
  // is missing cached runway information.
  bool needRunways = false;

  for (int i = 0; i < airportCount; i++) {
    if (!runwayData[i].loaded && runwayPrefs[i].mode != 2)
      needRunways = true;
  }

  if (needRunways) {
    lastRunwayAttempt = millis();

    bool ok = downloadRunwayDatabase();

    Serial.println(
      ok ?
      "Runway download complete" :
      "Runway download failed"
    );
  }

  drawCurrentAirport();

  Serial.println("METAR Display v4.5 ready");
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop() {

  // Pending restart
  if (restartAt != 0) {
    if ((long)(millis() - restartAt) >= 0)
      ESP.restart();

    if (setupMode) {
      dns.processNextRequest();
      server.handleClient();
    }

    delay(5);
    return;
  }

  // =================================================
  // SETUP MODE
  // Touch upper 75% to exit without saving.
  // =================================================

  if (setupMode) {
    bool down = touch.touched();

    if (down && !lastTouchDown) {
      TS_Point p = touch.getPoint();

      if (p.z > 100) {
        int screenY = constrain(
          map(p.y, 260, 3730, 0, 319),
          0, 319
        );

        if (screenY < 240) {
          Serial.println(
            "Exiting Setup Mode by touch"
          );

          statusMessage(
            "Leaving setup", "Restarting..."
          );

          ESP.restart();
        }
      }
    }

    lastTouchDown = down;

    dns.processNextRequest();
    server.handleClient();

    delay(5);
    return;
  }

  // =================================================
  // NORMAL WEATHER DISPLAY
  // =================================================

  handleTouch();

  if (setupMode) return;

  unsigned long now = millis();

  // Automatic airport rotation
  if (
    airportCount > 1 &&
    now - lastRotate >=
      (unsigned long)rotationSeconds * 1000UL
  ) {
    currentAirport =
      (currentAirport + 1) % airportCount;

    lastRotate = now;
    drawCurrentAirport();
  }

  // Wi-Fi recovery
  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWiFiRetry >= WIFI_RETRY_MS) {
      lastWiFiRetry = now;
      WiFi.reconnect();

      Serial.println("Reconnecting Wi-Fi...");
    }

    delay(20);
    return;
  }

  // NOAA weather refresh
  if (
    firstFetch ||
    now - lastFetch >= REFRESH_MS
  ) {
    firstFetch = false;
    lastFetch = now;

    if (!fetchAllMETARs()) {
      Serial.println("METAR download failed");

      lastFetch =
        millis() - (REFRESH_MS - RETRY_MS);
    }
  }

  // Retry missing runway information occasionally.
  // Existing runway cache is preserved.
  bool missingRunways = false;

  for (int i = 0; i < airportCount; i++) {
    if (!runwayData[i].loaded && runwayPrefs[i].mode != 2) {
      missingRunways = true;
      break;
    }
  }

  if (
    missingRunways &&
    now - lastRunwayAttempt >= RUNWAY_RETRY_MS
  ) {
    lastRunwayAttempt = now;

    downloadRunwayDatabase();
    drawCurrentAirport();
  }

  // Update the clock every minute
  time_t currentTime = time(nullptr);

  if (currentTime >= 1700000000) {
    long minuteNow = currentTime / 60;

    if (minuteNow != lastClockMinute) {
      lastClockMinute = minuteNow;

      if (metarCache[currentAirport].length()) {
        JsonDocument doc;

        if (!deserializeJson(
          doc, metarCache[currentAirport]
        )) {
          long obs = doc["obsTime"] | 0L;
          drawClockLine(obs);
        }
      }
    }
  }

  delay(20);
}
