/*
 Copy to secrets.h and enter your own values.
 NEVER commit secrets.h to a public repository.
*/
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Optional secondary Wi-Fi network; leave empty if unused.
const char* WIFI_SSID2 = "";
const char* WIFI_PASSWORD2 = "";

// Approximate location for Open-Meteo. Replace these values.
// Do not publish your precise home coordinates.
const float LATITUDE = 55.0;
const float LONGITUDE = 12.0;

// Home Assistant local address, without http:// or trailing slash.
const char* HA_HOST = "192.168.1.100";
const uint16_t HA_PORT = 8123;

// Create a dedicated Home Assistant long-lived access token.
// Treat it like a password.
const char* HA_TOKEN = "YOUR_HOME_ASSISTANT_LONG_LIVED_ACCESS_TOKEN";
