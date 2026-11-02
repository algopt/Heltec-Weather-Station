#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <esp_sleep.h>

#include "HT_E0213A367.h"
#include "secrets.h"
#include "sensors.h"

// ============================================================
// DISPLAY
// ============================================================

HT_E0213A367 display(6, 5, 4, 7, 3, 2, -1, 6000000);

#define DIRECTION ANGLE_0_DEGREE

// ============================================================
// WIFI
// ============================================================

WiFiMulti wifiMulti;

// ============================================================
// SLEEP TIMES
// ============================================================

const uint64_t SLEEP_30_MIN_US =
  30ULL * 60ULL * 1000000ULL;

const uint64_t SLEEP_2_HOUR_US =
  2ULL * 60ULL * 60ULL * 1000000ULL;

// ============================================================
// WEATHER DATA
// ============================================================

float weatherTemp = 0.0;
float weatherHumidity = 0.0;
float weatherWind = 0.0;

int weatherCode = 0;
String weatherDescription = "Unknown";

// ============================================================
// SOLAR DATA
// ============================================================

float solarToday = 0.0;
float solarTomorrow = 0.0;
float solarAfter = 0.0;
float solarAfter2 = 0.0;

// ============================================================
// HOME TEMPERATURES
// ============================================================

float tempNorth = 0.0;
float tempSouth = 0.0;
float tempBedroom = 0.0;
float tempGarage = 0.0;
float tempKitchen = 0.0;

// ============================================================
// BATTERY
// ============================================================

#define BATTERY_PIN 20
#define BATTERY_CTRL 19

float batteryVoltage = 0.0;
int batteryPercent = 0;

// ============================================================
// TIME
// ============================================================

String currentTime = "--:--";

const char* dayNames[] = {
  "Sun",
  "Mon",
  "Tue",
  "Wed",
  "Thu",
  "Fri",
  "Sat"
};

// ============================================================
// WEATHER DESCRIPTION
// ============================================================

String weatherCodeToText(int code)
{
  if (code == 0)
    return "Clear";

  if (code == 1 || code == 2)
    return "Partly Cloudy";

  if (code == 3)
    return "Cloudy";

  if (code >= 45 && code <= 48)
    return "Fog";

  if (code >= 51 && code <= 57)
    return "Drizzle";

  if (code >= 61 && code <= 67)
    return "Rain";

  if (code >= 71 && code <= 77)
    return "Snow";

  if (code >= 80 && code <= 82)
    return "Showers";

  if (code >= 85 && code <= 86)
    return "Snow Showers";

  if (code >= 95 && code <= 99)
    return "Thunder";

  return "Unknown";
}

// ============================================================
// SIMPLE JSON VALUE READER
// ============================================================
// Uses the LAST occurrence of the key.
// Open-Meteo includes the same keys in current_units and current.
// ============================================================

String getJsonValue(const String& json, const String& key)
{
  String search = "\"" + key + "\"";

  int pos = json.lastIndexOf(search);

  if (pos < 0)
    return "";

  pos = json.indexOf(":", pos);

  if (pos < 0)
    return "";

  pos++;

  while (pos < json.length() &&
         (json[pos] == ' ' || json[pos] == '"'))
  {
    pos++;
  }

  int end = pos;

  bool quoted = false;

  if (pos > 0 && json[pos - 1] == '"')
    quoted = true;

  if (quoted)
  {
    end = json.indexOf("\"", pos);

    if (end < 0)
      return "";

    return json.substring(pos, end);
  }

  while (end < json.length() &&
         json[end] != ',' &&
         json[end] != '}')
  {
    end++;
  }

  return json.substring(pos, end);
}

// ============================================================
// EXTRACT ONE ENTITY'S STATE FROM A FULL /api/states PAYLOAD
// ============================================================
// Finds "entity_id":"<entity>" in the array, then reads the
// "state" field that belongs to that same object (bounded by
// the start of the next object's entity_id, so it can't read
// past into a neighbouring entity).
// ============================================================

float extractStateForEntity(const String& json, const char* entity)
{
  String key =
    "\"entity_id\":\"" +
    String(entity) +
    "\"";

  int pos =
    json.indexOf(key);

  if (pos < 0)
  {
    Serial.print(
      "HA entity not found: "
    );

    Serial.println(
      entity
    );

    return 0.0;
  }

  int nextEntityPos =
    json.indexOf(
      "\"entity_id\":\"",
      pos + key.length()
    );

  if (nextEntityPos < 0)
    nextEntityPos = json.length();

  int statePos =
    json.indexOf(
      "\"state\":\"",
      pos
    );

  if (statePos < 0 || statePos > nextEntityPos)
  {
    Serial.print(
      "HA state field not found for: "
    );

    Serial.println(
      entity
    );

    return 0.0;
  }

  statePos += 9; // length of "\"state\":\""

  int end =
    json.indexOf(
      "\"",
      statePos
    );

  if (end < 0 || end > nextEntityPos)
    return 0.0;

  return json.substring(statePos, end).toFloat();
}

// ============================================================
// GET LOCAL TIME
// ============================================================

void updateTime()
{
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo))
  {
    currentTime = "--:--";
    return;
  }

  char buffer[10];

  strftime(
    buffer,
    sizeof(buffer),
    "%H:%M",
    &timeinfo
  );

  currentTime = String(buffer);
}

// ============================================================
// SYNCHRONISE LOCAL TIME
// ============================================================

bool syncTime()
{
  configTzTime(
    "CET-1CEST,M3.5.0,M10.5.0/3",
    "pool.ntp.org",
    "time.nist.gov"
  );

  Serial.println(
    "Synchronising time..."
  );

  struct tm timeinfo;

  // Wait up to 10 seconds for NTP
  if (!getLocalTime(&timeinfo, 10000))
  {
    Serial.println(
      "Time synchronisation failed"
    );

    // Fall back to current RTC time
    updateTime();

    return false;
  }

  char buffer[10];

  strftime(
    buffer,
    sizeof(buffer),
    "%H:%M",
    &timeinfo
  );

  currentTime = String(buffer);

  Serial.print(
    "Current time: "
  );

  Serial.println(
    currentTime
  );

  return true;
}

// ============================================================
// GET DAY NAME
// ============================================================

String getDayName(int offset)
{
  time_t now = time(nullptr);

  if (now < 100000)
    return "---";

  struct tm timeinfo;

  localtime_r(
    &now,
    &timeinfo
  );

  timeinfo.tm_mday += offset;

  mktime(&timeinfo);

  return String(
    dayNames[timeinfo.tm_wday]
  );
}

// ============================================================
// BATTERY
// ============================================================

void readBattery()
{
  // Battery ADC is on ADC2.
  // Must be read before Wi-Fi is started.

  pinMode(
    BATTERY_CTRL,
    OUTPUT
  );

  // LOW enables the battery measurement circuit.
  digitalWrite(
    BATTERY_CTRL,
    LOW
  );

  delay(20);

  analogSetPinAttenuation(
    BATTERY_PIN,
    ADC_11db
  );

  long total = 0;

  const int samples = 20;

  for (int i = 0; i < samples; i++)
  {
    total += analogRead(
      BATTERY_PIN
    );

    delay(2);
  }

  int raw =
    total / samples;

  // Calibrated against your multimeter.
  batteryVoltage =
    raw * 0.001747f;

  // Approximate Li-Po percentage.
  batteryPercent =
    (int)(
      ((batteryVoltage - 3.20f) /
       (4.20f - 3.20f)) *
      100.0f
    );

  batteryPercent =
    constrain(
      batteryPercent,
      0,
      100
    );

  Serial.print(
    "Battery raw: "
  );

  Serial.println(
    raw
  );

  Serial.print(
    "Battery voltage: "
  );

  Serial.print(
    batteryVoltage,
    3
  );

  Serial.println(
    " V"
  );

  Serial.print(
    "Battery: "
  );

  Serial.print(
    batteryPercent
  );

  Serial.println(
    "%"
  );

  // Disable battery measurement circuit.
  digitalWrite(
    BATTERY_CTRL,
    HIGH
  );
}

// ============================================================
// WEATHER FROM OPEN-METEO
// ============================================================

bool getWeather()
{
  HTTPClient http;

  String url =
    "https://api.open-meteo.com/v1/forecast?"
    "latitude=" + String(LATITUDE, 6) +
    "&longitude=" + String(LONGITUDE, 6) +
    "&current=temperature_2m,relative_humidity_2m,"
    "weather_code,wind_speed_10m"
    "&timezone=auto";

  Serial.println();
  Serial.println(
    "Requesting weather:"
  );

  Serial.println(
    url
  );

  http.begin(url);

  int httpCode =
    http.GET();

  Serial.print(
    "HTTP code: "
  );

  Serial.println(
    httpCode
  );

  if (httpCode != 200)
  {
    http.end();
    return false;
  }

  String payload =
    http.getString();

  http.end();

  String value;

  value =
    getJsonValue(
      payload,
      "temperature_2m"
    );

  if (value.length())
    weatherTemp =
      value.toFloat();

  value =
    getJsonValue(
      payload,
      "relative_humidity_2m"
    );

  if (value.length())
    weatherHumidity =
      value.toFloat();

  value =
    getJsonValue(
      payload,
      "wind_speed_10m"
    );

  if (value.length())
    weatherWind =
      value.toFloat();

  value =
    getJsonValue(
      payload,
      "weather_code"
    );

  if (value.length())
    weatherCode =
      value.toInt();

  weatherDescription =
    weatherCodeToText(
      weatherCode
    );

  Serial.println(
    "Weather received"
  );

  Serial.printf(
    "Temperature: %.2f\n",
    weatherTemp
  );

  Serial.printf(
    "Humidity: %.2f\n",
    weatherHumidity
  );

  Serial.printf(
    "Wind: %.2f\n",
    weatherWind
  );

  Serial.print(
    "Weather code: "
  );

  Serial.println(
    weatherCode
  );

  Serial.print(
    "Weather: "
  );

  Serial.println(
    weatherDescription
  );

  return true;
}

// ============================================================
// GET ALL HA SENSOR VALUES VIA TEMPLATE API (ONE REQUEST)
// ============================================================
// Asks HA to render exactly the 9 values we need, in order,
// separated by "|". Avoids pulling every entity in the
// instance and searching it locally.
// ============================================================

bool getHomeAssistantTemplateValues(String& payloadOut)
{
  HTTPClient http;

  String url =
    "http://" +
    String(HA_HOST) +
    ":" +
    String(HA_PORT) +
    "/api/template";

  String templateBody =
    String("{\"template\":\"") +
    "{{ states('" + HA_SOLAR_TODAY + "') }}|" +
    "{{ states('" + HA_SOLAR_TOMORROW + "') }}|" +
    "{{ states('" + HA_SOLAR_AFTER + "') }}|" +
    "{{ states('" + HA_SOLAR_AFTERX2 + "') }}|" +
    "{{ states('" + HA_TEMP_NORTH + "') }}|" +
    "{{ states('" + HA_TEMP_SOUTH + "') }}|" +
    "{{ states('" + HA_TEMP_BEDROOM + "') }}|" +
    "{{ states('" + HA_TEMP_GARAGE + "') }}|" +
    "{{ states('" + HA_TEMP_KITCHEN + "') }}" +
    "\"}";

  Serial.println();
  Serial.println(
    "Requesting Home Assistant values via template..."
  );

  http.begin(url);

  http.addHeader(
    "Authorization",
    "Bearer " +
    String(HA_TOKEN)
  );

  http.addHeader(
    "Content-Type",
    "application/json"
  );

  int httpCode =
    http.POST(templateBody);

  Serial.print(
    "HA /api/template HTTP code: "
  );

  Serial.println(
    httpCode
  );

  if (httpCode != 200)
  {
    Serial.println(
      "HA template request failed."
    );

    http.end();
    return false;
  }

  payloadOut =
    http.getString();

  http.end();

  Serial.print(
    "HA template response: "
  );

  Serial.println(
    payloadOut
  );

  return true;
}

// ============================================================
// GET ALL HA DATA
// ============================================================

void getHomeAssistantData()
{
  Serial.println();
  Serial.println(
    "Reading Home Assistant sensors..."
  );

  String payload;

  if (!getHomeAssistantTemplateValues(payload))
  {
    Serial.println(
      "Failed to fetch Home Assistant values; "
      "keeping previous values."
    );

    return;
  }

  // Split on "|" in order: solar x4, then temps x5.

  float values[9];
  int valueIndex = 0;
  int start = 0;

  for (int i = 0; i <= payload.length() && valueIndex < 9; i++)
  {
    if (i == payload.length() || payload[i] == '|')
    {
      values[valueIndex] =
        payload.substring(start, i).toFloat();

      valueIndex++;
      start = i + 1;
    }
  }

  if (valueIndex < 9)
  {
    Serial.printf(
      "HA template response had only %d of 9 expected values; "
      "keeping previous values.\n",
      valueIndex
    );

    return;
  }

  solarToday     = values[0];
  solarTomorrow  = values[1];
  solarAfter     = values[2];
  solarAfter2    = values[3];
  tempNorth      = values[4];
  tempSouth      = values[5];
  tempBedroom    = values[6];
  tempGarage     = values[7];
  tempKitchen    = values[8];

  Serial.printf(
    "Solar: %.1f / %.1f / %.1f / %.1f kWh\n",
    solarToday,
    solarTomorrow,
    solarAfter,
    solarAfter2
  );

  Serial.printf(
    "Temps: %.1f %.1f %.1f %.1f %.1f\n",
    tempNorth,
    tempSouth,
    tempBedroom,
    tempGarage,
    tempKitchen
  );
}

// ============================================================
// CLOUD OUTLINE
// ============================================================
// Same outlined cloud shape is used by all cloud-based icons.
// Approx. same footprint as the previous filled cloud.
// ============================================================

void drawCloudOutline(
  int x,
  int y
)
{
  // Main outer contour

  display.drawLine(
    x + 13,
    y + 28,
    x + 12,
    y + 25
  );

  display.drawLine(
    x + 12,
    y + 25,
    x + 13,
    y + 22
  );

  display.drawLine(
    x + 13,
    y + 22,
    x + 15,
    y + 19
  );

  display.drawLine(
    x + 15,
    y + 19,
    x + 18,
    y + 17
  );

  display.drawLine(
    x + 18,
    y + 17,
    x + 21,
    y + 17
  );

  display.drawLine(
    x + 21,
    y + 17,
    x + 24,
    y + 19
  );

  // Large upper lobe

  display.drawLine(
    x + 24,
    y + 19,
    x + 25,
    y + 16
  );

  display.drawLine(
    x + 25,
    y + 16,
    x + 27,
    y + 13
  );

  display.drawLine(
    x + 27,
    y + 13,
    x + 30,
    y + 11
  );

  display.drawLine(
    x + 30,
    y + 11,
    x + 34,
    y + 10
  );

  display.drawLine(
    x + 34,
    y + 10,
    x + 38,
    y + 10
  );

  display.drawLine(
    x + 38,
    y + 10,
    x + 41,
    y + 12
  );

  display.drawLine(
    x + 41,
    y + 12,
    x + 44,
    y + 15
  );

  display.drawLine(
    x + 44,
    y + 15,
    x + 46,
    y + 19
  );

  // Right lobe

  display.drawLine(
    x + 46,
    y + 19,
    x + 49,
    y + 19
  );

  display.drawLine(
    x + 49,
    y + 19,
    x + 52,
    y + 20
  );

  display.drawLine(
    x + 52,
    y + 20,
    x + 54,
    y + 22
  );

  display.drawLine(
    x + 54,
    y + 22,
    x + 56,
    y + 25
  );

  display.drawLine(
    x + 56,
    y + 25,
    x + 56,
    y + 28
  );

  display.drawLine(
    x + 56,
    y + 28,
    x + 55,
    y + 30
  );

  display.drawLine(
    x + 55,
    y + 30,
    x + 52,
    y + 32
  );

  // Bottom

  display.drawLine(
    x + 52,
    y + 32,
    x + 18,
    y + 32
  );

  display.drawLine(
    x + 18,
    y + 32,
    x + 15,
    y + 31
  );

  display.drawLine(
    x + 15,
    y + 31,
    x + 13,
    y + 29
  );

  display.drawLine(
    x + 13,
    y + 29,
    x + 13,
    y + 28
  );

  // Small internal curve / 3D detail

  display.drawLine(
    x + 17,
    y + 25,
    x + 17,
    y + 27
  );

  display.drawLine(
    x + 17,
    y + 27,
    x + 19,
    y + 29
  );

  display.drawLine(
    x + 19,
    y + 29,
    x + 22,
    y + 30
  );

  display.drawLine(
    x + 22,
    y + 30,
    x + 26,
    y + 30
  );
}

// ============================================================
// WEATHER ICONS
// ============================================================

void drawWeatherIcon(
  int x,
  int y
)
{
  // ----------------------------------------------------------
  // CLEAR / SUNNY
  // WMO 0
  // ----------------------------------------------------------

  if (weatherCode == 0)
  {
    display.fillCircle(
      x + 26,
      y + 20,
      10
    );

    for (int i = 0; i < 8; i++)
    {
      float a =
        i * PI / 4.0;

      int x1 =
        x + 26 +
        cos(a) * 14;

      int y1 =
        y + 20 +
        sin(a) * 14;

      int x2 =
        x + 26 +
        cos(a) * 20;

      int y2 =
        y + 20 +
        sin(a) * 20;

      display.drawLine(
        x1,
        y1,
        x2,
        y2
      );
    }

    return;
  }

  // ----------------------------------------------------------
  // PARTLY CLOUDY
  // WMO 1-2
  // ----------------------------------------------------------

  if (weatherCode == 1 ||
      weatherCode == 2)
  {
    // Sun

    display.fillCircle(
      x + 15,
      y + 14,
      7
    );

    // Sun rays

    for (int i = 0; i < 8; i++)
    {
      float a =
        i * PI / 4.0;

      int x1 =
        x + 15 +
        cos(a) * 10;

      int y1 =
        y + 14 +
        sin(a) * 10;

      int x2 =
        x + 15 +
        cos(a) * 14;

      int y2 =
        y + 14 +
        sin(a) * 14;

      display.drawLine(
        x1,
        y1,
        x2,
        y2
      );
    }

    // Same cloud shape

    drawCloudOutline(
      x + 5,
      y + 3
    );

    return;
  }

  // ----------------------------------------------------------
  // CLOUDY / OVERCAST
  // WMO 3
  // ----------------------------------------------------------

  if (weatherCode == 3)
  {
    drawCloudOutline(
      x,
      y
    );

    return;
  }

  // ----------------------------------------------------------
  // FOG
  // WMO 45-48
  // ----------------------------------------------------------

  if (weatherCode >= 45 &&
      weatherCode <= 48)
  {
    drawCloudOutline(
      x,
      y
    );

    // Horizontal fog lines

    display.drawLine(
      x + 10,
      y + 36,
      x + 56,
      y + 36
    );

    display.drawLine(
      x + 15,
      y + 42,
      x + 51,
      y + 42
    );

    display.drawLine(
      x + 10,
      y + 48,
      x + 56,
      y + 48
    );

    return;
  }

  // ----------------------------------------------------------
  // RAIN / DRIZZLE / SHOWERS
  // WMO 51-67, 80-82
  // ----------------------------------------------------------

  if ((weatherCode >= 51 &&
       weatherCode <= 67) ||
      (weatherCode >= 80 &&
       weatherCode <= 82))
  {
    drawCloudOutline(
      x,
      y
    );

    // Rain

    display.drawLine(
      x + 20,
      y + 36,
      x + 16,
      y + 44
    );

    display.drawLine(
      x + 34,
      y + 36,
      x + 30,
      y + 44
    );

    display.drawLine(
      x + 48,
      y + 36,
      x + 44,
      y + 44
    );

    return;
  }

  // ----------------------------------------------------------
  // SNOW
  // WMO 71-77, 85-86
  // ----------------------------------------------------------

  if ((weatherCode >= 71 &&
       weatherCode <= 77) ||
      (weatherCode >= 85 &&
       weatherCode <= 86))
  {
    drawCloudOutline(
      x,
      y
    );

    display.drawString(
      x + 16,
      y + 34,
      "*"
    );

    display.drawString(
      x + 30,
      y + 34,
      "*"
    );

    display.drawString(
      x + 44,
      y + 34,
      "*"
    );

    return;
  }

  // ----------------------------------------------------------
  // THUNDERSTORM
  // WMO 95-99
  // ----------------------------------------------------------

  if (weatherCode >= 95)
  {
    drawCloudOutline(
      x,
      y
    );

    // Lightning

    display.drawLine(
      x + 34,
      y + 31,
      x + 27,
      y + 40
    );

    display.drawLine(
      x + 27,
      y + 40,
      x + 34,
      y + 40
    );

    display.drawLine(
      x + 34,
      y + 40,
      x + 29,
      y + 51
    );

    return;
  }
}

// ============================================================
// DRAW DISPLAY
// ============================================================

void drawDisplay()
{
  display.clear();

  display.setTextAlignment(
    TEXT_ALIGN_LEFT
  );

  // ----------------------------------------------------------
  // TOP
  // ----------------------------------------------------------

  display.setFont(
    ArialMT_Plain_16
  );

  display.drawString(
    8,
    2,
    "WEATHER"
  );

  display.setFont(
    ArialMT_Plain_24
  );

  display.drawString(
    8,
    23,
    String(weatherTemp, 1) +
    " C"
  );

  // Time — upper right

  display.setFont(
    ArialMT_Plain_10
  );

  display.drawString(
    202,
    4,
    currentTime
  );

  // Weather icon

  drawWeatherIcon(
    150,
    5
  );

  // ----------------------------------------------------------
  // HUMIDITY + WIND + STATUS
  // ----------------------------------------------------------

  display.setFont(
    ArialMT_Plain_10
  );

  display.drawString(
    8,
    51,
    "Humidity " +
    String(weatherHumidity, 0) +
    "%"
  );

  display.drawString(
    80,
    51,
    "Wind " +
    String(weatherWind, 1) +
    " km/h"
  );

  display.drawString(
    154,
    51,
    weatherDescription
  );

  // ----------------------------------------------------------
  // SEPARATOR
  // ----------------------------------------------------------

  display.drawLine(
    8,
    63,
    242,
    63
  );

  // ----------------------------------------------------------
  // SOLAR
  // ----------------------------------------------------------

  display.setFont(
    ArialMT_Plain_10
  );

  display.drawString(
    8,
    63,
    "SOLAR kWh/day"
  );

  // Day labels

  display.drawString(
    96,
    63,
    "Today"
  );

  display.drawString(
    136,
    63,
    getDayName(1)
  );

  display.drawString(
    176,
    63,
    getDayName(2)
  );

  display.drawString(
    216,
    63,
    getDayName(3)
  );

  // Values

  display.drawString(
    96,
    72,
    String(solarToday, 1)
  );

  display.drawString(
    136,
    72,
    String(solarTomorrow, 1)
  );

  display.drawString(
    176,
    72,
    String(solarAfter, 1)
  );

  display.drawString(
    216,
    72,
    String(solarAfter2, 1)
  );

  // ----------------------------------------------------------
  // SECOND SEPARATOR
  // ----------------------------------------------------------

  display.drawLine(
    8,
    83,
    242,
    83
  );

  // ----------------------------------------------------------
  // HOME TEMPERATURES
  // ----------------------------------------------------------

  display.setFont(
    ArialMT_Plain_10
  );

  display.drawString(
    8,
    84,
    "HOME TEMPERATURES"
  );

  // Row 1

  display.drawString(
    8,
    94,
    "North " +
    String(tempNorth, 1) +
    " C"
  );

  display.drawString(
    126,
    94,
    "South " +
    String(tempSouth, 1) +
    " C"
  );

  // Row 2

  display.drawString(
    8,
    103,
    "Bedroom " +
    String(tempBedroom, 1) +
    " C"
  );

  display.drawString(
    126,
    103,
    "Garage " +
    String(tempGarage, 1) +
    " C"
  );

  // Row 3

  display.drawString(
    8,
    112,
    "Kitchen " +
    String(tempKitchen, 1) +
    " C"
  );

  // Battery lower right

  display.drawString(
    166,
    112,
    "Batt: " +
    String(batteryVoltage, 2) +
    "V " +
    String(batteryPercent) +
    "%"
  );

  // ----------------------------------------------------------
  // SEND FRAMEBUFFER TO E-PAPER
  // ----------------------------------------------------------

  display.update(
    BLACK_BUFFER
  );

  display.display();
}

// ============================================================
// WIFI CONNECTION
// ============================================================

bool connectWiFi()
{
  Serial.println();
  Serial.println(
    "Connecting to WiFi..."
  );

  wifiMulti.addAP(
    WIFI_SSID,
    WIFI_PASSWORD
  );

  wifiMulti.addAP(
    WIFI_SSID2,
    WIFI_PASSWORD2
  );

  unsigned long start =
    millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - start < 30000
  )
  {
    wifiMulti.run();

    Serial.print(".");

    delay(500);
  }

  Serial.println();

  if (
    WiFi.status() != WL_CONNECTED
  )
  {
    Serial.println(
      "WiFi connection failed"
    );

    return false;
  }

  Serial.println(
    "WiFi connected"
  );

  Serial.print(
    "SSID: "
  );

  Serial.println(
    WiFi.SSID()
  );

  Serial.print(
    "IP: "
  );

  Serial.println(
    WiFi.localIP()
  );

  return true;
}

// ============================================================
// GO TO DEEP SLEEP
// ============================================================

void goToSleep()
{
  Serial.println();
  Serial.println(
    "Preparing for deep sleep..."
  );

  // Stop Wi-Fi completely

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  delay(100);

  // Disable battery measurement circuit

  pinMode(
    BATTERY_CTRL,
    OUTPUT
  );

  digitalWrite(
    BATTERY_CTRL,
    HIGH
  );

  // Turn off display/peripheral power.
  // E-paper keeps its image.

  digitalWrite(
    Vext,
    HIGH
  );

  delay(100);

  // ----------------------------------------------------------
  // CALCULATE NEXT ROUND UPDATE TIME
  // ----------------------------------------------------------

  struct tm timeinfo;

  uint64_t sleepTimeUs;

  time_t now =
    time(nullptr);

  if (
    now >= 100000 &&
    localtime_r(
      &now,
      &timeinfo
    )
  )
  {
    struct tm target =
      timeinfo;

    target.tm_sec = 0;

    // --------------------------------------------------------
    // NIGHT: 00:00 - 05:59
    // Updates at 00:00, 02:00, 04:00, 06:00
    // --------------------------------------------------------

    if (
      timeinfo.tm_hour < 6
    )
    {
      target.tm_min = 0;

      int nextHour =
        ((timeinfo.tm_hour / 2) + 1) * 2;

      if (nextHour > 6)
        nextHour = 6;

      target.tm_hour =
        nextHour;

      Serial.printf(
        "Night mode: next update at %02d:%02d\n",
        target.tm_hour,
        target.tm_min
      );
    }

    // --------------------------------------------------------
    // DAY: 06:00 - 23:59
    // Updates every 90 minutes (00:00, 01:30, 03:00, ...),
    // so whatever slot falls after 06:00 is the first one used.
    // --------------------------------------------------------

    else
    {
      int totalMinutes =
        timeinfo.tm_hour * 60 +
        timeinfo.tm_min;

      int nextSlot =
        ((totalMinutes / 90) + 1) * 90;

      target.tm_hour = nextSlot / 60;
      target.tm_min = nextSlot % 60;

      Serial.printf(
        "Day mode: next update at %02d:%02d\n",
        target.tm_hour,
        target.tm_min
      );
    }

    time_t targetTime =
      mktime(
        &target
      );

    int64_t secondsUntil =
      (int64_t)difftime(
        targetTime,
        now
      );

    // Safety check

    if (secondsUntil < 10)
      secondsUntil = 10;

    sleepTimeUs =
      (uint64_t)secondsUntil *
      1000000ULL;

    Serial.print(
      "Sleeping for "
    );

    Serial.print(
      secondsUntil
    );

    Serial.println(
      " seconds."
    );
  }
  else
  {
    // Fallback if time is unavailable

    sleepTimeUs =
      30ULL *
      60ULL *
      1000000ULL;

    Serial.println(
      "Time unavailable: sleeping for 30 minutes."
    );
  }

  // ----------------------------------------------------------
  // WAKE TIMER
  // ----------------------------------------------------------

  esp_sleep_enable_timer_wakeup(
    sleepTimeUs
  );

  Serial.println(
    "Going to deep sleep."
  );

  Serial.flush();

  esp_deep_sleep_start();
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
  Serial.begin(
    115200
  );

  delay(500);

  Serial.println();
  Serial.println(
    "=== Heltec Weather Station ==="
  );

  // ----------------------------------------------------------
  // DISPLAY POWER
  // ----------------------------------------------------------

  pinMode(
    Vext,
    OUTPUT
  );

  digitalWrite(
    Vext,
    LOW
  );

  delay(100);

  // ----------------------------------------------------------
  // DISPLAY
  // ----------------------------------------------------------
  // No boot splash here: the first real e-paper refresh is
  // drawDisplay() once there's actual data to show. This
  // halves the number of full refreshes per wake cycle.

  display.init();

  display.screenRotate(
    DIRECTION
  );

  // ----------------------------------------------------------
  // BATTERY
  // ----------------------------------------------------------
  // Must be read before Wi-Fi because GPIO20 is ADC2.

  readBattery();

  // ----------------------------------------------------------
  // WIFI
  // ----------------------------------------------------------

  if (!connectWiFi())
  {
    display.clear();

    display.setFont(
      ArialMT_Plain_10
    );

    display.drawString(
      8,
      20,
      "WiFi connection failed"
    );

    display.update(
      BLACK_BUFFER
    );

    display.display();

    delay(3000);

    goToSleep();
  }

  // ----------------------------------------------------------
  // TIME
  // ----------------------------------------------------------
  // Synchronise every time we wake.

  syncTime();

  // ----------------------------------------------------------
  // GET DATA
  // ----------------------------------------------------------

  getWeather();

  getHomeAssistantData();

  // ----------------------------------------------------------
  // DRAW FINAL DISPLAY
  // ----------------------------------------------------------

  drawDisplay();

  Serial.println();
  Serial.println(
    "Update complete."
  );

  // Allow e-paper refresh to finish.

  delay(2000);

  // ----------------------------------------------------------
  // DEEP SLEEP
  // ----------------------------------------------------------

  goToSleep();
}

// ============================================================
// LOOP
// ============================================================

void loop()
{
  // Never reached.
  // ESP32 restarts setup() after deep-sleep wake.
}