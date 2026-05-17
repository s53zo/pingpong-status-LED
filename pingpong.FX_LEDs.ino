#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#define MQTT_MAX_PACKET_SIZE 2048
#include <PubSubClient.h>
#include <EEPROM.h>
#include <WS2812FX.h>
#include <ArduinoJson.h>
#include <Ticker.h>
#include <vector> 
#include <map>
#include <Wire.h>

#include "antenna_mqtt_handler.h"
#include "dongutec_keypad_mcp23008.h"

// WiFi + MQTT setup (two brokers):
// - Pingpong broker: LED commands + debug topic
// - MatriGS broker: station/band state + antenna switching
WiFiClient espClientCmd;
WiFiClient espClientMatrigs;
PubSubClient clientCmd(espClientCmd);
PubSubClient clientMatrigs(espClientMatrigs);
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

// Constants
#define VER "v2.16 may2026 rxtx"
#define LED_PIN D3
#define NUM_LEDS 4
#define AP_SSID "ESP8266_Setup"
WS2812FX ws2812fx = WS2812FX(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// -----------------------------------------------------------------
//  External keypad (Dongutec 4x4 via MCP23008 on I2C)
//  Default I2C address is 0x27 (can be changed via address pads).
// -----------------------------------------------------------------
static constexpr uint8_t  KEYPAD_I2C_ADDR      = 0x27;
static constexpr uint8_t  KEYPAD_SDA_PIN       = D2;     // GPIO4
static constexpr uint8_t  KEYPAD_SCL_PIN       = D1;     // GPIO5
static constexpr uint32_t KEYPAD_POLL_MS       = 20;     // poll cadence
static constexpr uint32_t KEYPAD_DEBOUNCE_MS   = 50;     // stable time before press event

DongutecKeypadMcp23008 g_keypad(KEYPAD_I2C_ADDR);
bool g_keypadPresent = false;
uint8_t g_keypadI2cAddr = 0;  // 0 => unknown/absent

// EEPROM layout (512 bytes reserved)
static constexpr int EEPROM_SIZE = 512;
static constexpr int EEPROM_OFF_SSID          = 0;    // char[32]
static constexpr int EEPROM_OFF_PASSWORD      = 32;   // char[32]
static constexpr int EEPROM_OFF_MQTT_SERVER   = 64;   // char[40] (Pingpong broker)
static constexpr int EEPROM_OFF_MQTT_PORT     = 104;  // int
static constexpr int EEPROM_OFF_STATION       = 108;  // char[32]
static constexpr int EEPROM_OFF_MATRIGS_SERVER = 140; // char[40]
static constexpr int EEPROM_OFF_MATRIGS_PORT   = 180; // int
static constexpr int EEPROM_OFF_MAGIC          = 200; // uint32_t
static constexpr uint32_t EEPROM_MAGIC_V2      = 0x50494E47; // "PING" marker

// Buffers
char ssid[32], password[32], mqtt_server[40], station_name[32];
int mqtt_port = 1883;  // Pingpong broker port
char matrigs_server[40] = "";
int  matrigs_port = 4883;  // MatriGS default (can be overridden in web UI)
char logBuffer[256];

char macAddress[18]     = "";          
char command_topic[64]  = "";
char feedback_topic[64] = "";
char debug_topic[64]    = "";
char currentRXTX[4] = "RX";     

char topic_band_prefix[64];      // NEW: "matrigs/0/sta/<sta>/b/"
char topic_band_wildcard[64];    // NEW: "…/b/#"

bool        mqttHelloRunning = false;
unsigned long mqttHelloStart = 0;

/* ---------- live TX/RX flag (true = PTT active) --------------- */
bool g_txActive = false;

/* ---------- pending TX change instance ------------------------ */
PendingTxChange g_pendingTx = { "", "", "", false };



// -----------------------------------------------------------------
//  Debug output master switch – starts OFF
// -----------------------------------------------------------------
bool debugEnabled = true;          // ← start verbose
unsigned long debugDeadline = 60000; 

String currentBand = "?";
String currentAntennas = "?";
std::vector<String> currentAntList;
std::map<String,String> bandCache;    // band ➜ sorted antenna list

char topic_cmd[128];
char topic_station_state[128];
char topic_available[128];
char topic_dt[128];

// Forward declarations (used in MQTT callback)
static void enableDebugFor1Minute(const char* source);

// -----------------------------------------------------------------
//  Debug helper – only speaks when debugEnabled == true
// -----------------------------------------------------------------
void publishDebugMessage(const char* msg) {
  if (!debugEnabled) return;              // ← early exit

  Serial.println(msg);

  if (clientCmd.connected() && debug_topic[0])
    clientCmd.publish(debug_topic, msg);
}

// Publish even when debugEnabled is off (use sparingly for user-triggered events).
static void publishDebugMessageAlways(const char* msg)
{
  Serial.println(msg);
  if (clientCmd.connected() && debug_topic[0])
    clientCmd.publish(debug_topic, msg);
}

// --- Web config ---
/* -------- static chunks in PROGMEM -------------------------------- */
const char PAGE_HEAD[] PROGMEM =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<title>ESP8266 Station Config</title><style>"
"body{font-family:Arial,Helvetica,sans-serif;margin:20px;color:#222;}"
"h1{color:#006edc;} input[type=text],input[type=number]{width:260px;}"
"input[type=submit]{padding:6px 18px;margin-top:10px;}"
"code{background:#f2f2f2;padding:2px 4px;border-radius:3px;}"
"</style></head><body>"
"<h1>Wi-Fi & MQTT Config</h1>";

const char PAGE_FORM1[] PROGMEM =
"<form action='/save' method='post'>"
"SSID:<br><input name='ssid' type='text' value='";

const char PAGE_FORM2[] PROGMEM =
"'><br>Password:<br><input name='password' type='text' value='";

const char PAGE_FORM3[] PROGMEM =
"'><br>MQTT Server IP:<br><input name='mqtt_server' type='text' value='";

const char PAGE_FORM4[] PROGMEM =
"'><br>MQTT Port:<br><input name='mqtt_port' type='number' value='";

const char PAGE_FORM5[] PROGMEM =
"'><br>Station Name (RTX-XX):<br><input name='station_name' type='text' value='";

const char PAGE_FORM6[] PROGMEM =
"'><br><input type='submit' value='Save & Reboot'></form>";

const char PAGE_STATUS_HEAD[] PROGMEM =
"<h2>Status</h2><ul>";

const char PAGE_FOOT[] PROGMEM =
"</ul><h2>OTA Update</h2>"
"<p>Open <a href='/update'>/update</a> to upload a new .bin.</p></body></html>";

const char PAGE_CHEAT[] PROGMEM =
"<h2>LED Command Cheat-Sheet</h2>"
"<p>Publish to <code>";

const char PAGE_CHEAT_TAIL[] PROGMEM =
"</code> with one of:</p><ul>"
"<li><code>0,255,100,FF0000</code> – static red</li>"
"<li><code>1,128,200,00FF00</code> – blink green</li>"
"<li><code>stop</code> – turn LEDs off</li>"
"<li><code>segment,0,2,255,100,FF0000</code> – single-pixel demo</li>"
"</ul>"
"<p>See the "
"<a href='https://github.com/kitesurfer1404/WS2812FX' target='_blank'>WS2812FX docs</a> "
"for all effect numbers.</p>";


// -------- send helper ---------------------------------------------
static void send_P(const char* progmem)
{
  server.sendContent_P(progmem, strlen_P(progmem));
}

/* -------- handleRoot() – no truncation, no heap bloat ------------ */
void handleRoot()
{
  publishDebugMessage("[HTTP] root");

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");   // start chunked reply

  /* --- static header --- */
  send_P(PAGE_HEAD);

  /* --- full <form> in one go (needs ~350 B worst-case) --- */
  char form[768];   // plenty of head-room
  snprintf(form, sizeof(form),
           "<form action='/save' method='post'>"
           "SSID:<br><input name='ssid' type='text' value='%s'><br>"
           "Password:<br><input name='password' type='text' value='%s'><br>"
           "Pingpong MQTT Server IP:<br><input name='mqtt_server' type='text' value='%s'><br>"
           "Pingpong MQTT Port:<br><input name='mqtt_port' type='number' value='%d'><br>"
           "MatriGS MQTT Server IP:<br><input name='matrigs_server' type='text' value='%s'><br>"
           "MatriGS MQTT Port:<br><input name='matrigs_port' type='number' value='%d'><br>"
           "Station Name (RTX-XX):<br><input name='station_name' type='text' value='%s'><br>"
           "<input type='submit' value='Save & Reboot'></form>",
           ssid, password, mqtt_server, mqtt_port, matrigs_server, matrigs_port, station_name);
  server.sendContent(form);

  /* --- status list --- */
  send_P(PAGE_STATUS_HEAD);

  char line[256];   // each individual <li> fits easily

  snprintf(line, sizeof(line), "<li>Firmware: <b>%s</b></li>", VER);
  server.sendContent(line);

  snprintf(line, sizeof(line),
           "<li>MAC Address: <code>%s</code></li>", macAddress);
  server.sendContent(line);

  snprintf(line, sizeof(line),
           "<li>Station: <b>%s</b></li>", station_name);
  server.sendContent(line);

  snprintf(line, sizeof(line),
           "<li>Pingpong MQTT: <code>%s:%d</code></li>", mqtt_server, mqtt_port);
  server.sendContent(line);

  snprintf(line, sizeof(line),
           "<li>MatriGS MQTT: <code>%s:%d</code></li>", matrigs_server, matrigs_port);
  server.sendContent(line);

  if (g_keypadPresent) {
    snprintf(line, sizeof(line),
             "<li>Keypad (MCP23008): <b>present</b> (0x%02X)</li>",
             g_keypadI2cAddr);
  } else {
    snprintf(line, sizeof(line),
             "<li>Keypad (MCP23008): <b>absent</b></li>");
  }
  server.sendContent(line);

  snprintf(line, sizeof(line),
           "<li>Current band: %s</li>", currentBand.c_str());
  server.sendContent(line);

  snprintf(line, sizeof(line),
           "<li>Selected antenna bank: %s</li>", currentRXTX);
  server.sendContent(line);

  snprintf(line, sizeof(line),
           "<li>Antennas for band: %s</li>", currentAntennas.c_str());
  server.sendContent(line);

  snprintf(line, sizeof(line),
           "<li>MQTT command topic: <code>%s</code></li>", command_topic);
  server.sendContent(line);

  snprintf(line, sizeof(line),
           "<li>MQTT feedback topic: <code>%s</code></li>", feedback_topic);
  server.sendContent(line);

  snprintf(line, sizeof(line),
           "<li>Debug topic: <code>%s</code></li>", debug_topic);
  server.sendContent(line);

  if (debugEnabled) {
    long msLeft = (debugDeadline > 0) ? (long)(debugDeadline - millis()) : 0;
    if (msLeft < 0) msLeft = 0;
    snprintf(line, sizeof(line),
             "<li>Debug: <b>ON</b> (%lds remaining)</li>", msLeft / 1000);
  } else {
    snprintf(line, sizeof(line),
             "<li>Debug: <b>OFF</b> (send MQTT <code>debug</code> to fxcmd)</li>");
  }
  server.sendContent(line);

  /* --- LED cheat-sheet --- */
  send_P(PAGE_CHEAT);
  server.sendContent(command_topic);
  send_P(PAGE_CHEAT_TAIL);

  /* --- footer & terminator --- */
  send_P(PAGE_FOOT);
  server.sendContent("");              // 0-length chunk = end of body
}



void handleUpdate() {
  publishDebugMessage("Serving OTA wrapper page");
  server.send(200, "text/html",
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<title>OTA Update</title></head><body>"
    "<h1>Firmware Upload</h1>"
    "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file'   name='update'><br><br>"
      "<input type='submit' value='Upload & Flash'>"
    "</form>"
    "<p>The device will reboot automatically after a successful upload.</p>"
    "</body></html>");
}

void handleSave() {
  strncpy(ssid, server.arg("ssid").c_str(), sizeof(ssid));
  strncpy(password, server.arg("password").c_str(), sizeof(password));
  strncpy(mqtt_server, server.arg("mqtt_server").c_str(), sizeof(mqtt_server));
  mqtt_port = server.arg("mqtt_port").toInt();
  strncpy(matrigs_server, server.arg("matrigs_server").c_str(), sizeof(matrigs_server));
  matrigs_port = server.arg("matrigs_port").toInt();
  strncpy(station_name, server.arg("station_name").c_str(), sizeof(station_name));

  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(EEPROM_OFF_SSID, ssid);
  EEPROM.put(EEPROM_OFF_PASSWORD, password);
  EEPROM.put(EEPROM_OFF_MQTT_SERVER, mqtt_server);
  EEPROM.put(EEPROM_OFF_MQTT_PORT, mqtt_port);
  EEPROM.put(EEPROM_OFF_STATION, station_name);

  EEPROM.put(EEPROM_OFF_MATRIGS_SERVER, matrigs_server);
  EEPROM.put(EEPROM_OFF_MATRIGS_PORT, matrigs_port);
  EEPROM.put(EEPROM_OFF_MAGIC, EEPROM_MAGIC_V2);
  EEPROM.commit();
  EEPROM.end();

  server.send(200, "text/html", "<html><body><h2>Saved. Rebooting...</h2></body></html>");
  delay(1000);
  ESP.restart();
}

// --- WiFi Setup ---
/* ================================================================
 *  Bring up Wi-Fi, then build all MQTT topics.
 *  (Subscriptions happen after broker connect.)
 * ===============================================================*/
void setupWiFi()
{
    delay(10);
    Serial.println();
    Serial.printf("Connecting to \"%s\" …\n", ssid);

    /* ---------- Wi-Fi in STA mode ----------------------------------- */
    WiFi.mode(WIFI_STA);
#if defined(ESP32)
    WiFi.setHostname(station_name);
#else
    WiFi.hostname(station_name);
#endif
    WiFi.begin(ssid, password);

    /* ---------- wait up to 30 s for a connection -------------------- */
    const uint32_t CONNECT_TIMEOUT_MS = 30'000;
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - t0 < CONNECT_TIMEOUT_MS) {
        delay(500);
        Serial.print('.');
    }

    /* ---------- fall back to AP if still not connected -------------- */
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\nTimeout – starting AP fallback");
        setupAP();                                // your existing helper
        return;                                   // skip the rest
    }

    /* ---------- success! ------------------------------------------- */
    Serial.println();
    Serial.print("Connected! IP = ");
    Serial.println(WiFi.localIP());

    /* ---------- MAC address as ASCII “AA:BB:CC:DD:EE:FF” ----------- */
#if defined(ESP32)
    uint8_t rawMac[6];
    WiFi.macAddress(rawMac);                      // fill 6-byte array
    snprintf(macAddress, sizeof(macAddress),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             rawMac[0], rawMac[1], rawMac[2],
             rawMac[3], rawMac[4], rawMac[5]);
#else                                             // ESP8266
    String macStr = WiFi.macAddress();            // returns String
    macStr.toCharArray(macAddress, sizeof(macAddress));
#endif

    /* ---------- build MQTT topic strings --------------------------- */
    snprintf(command_topic,  sizeof(command_topic),
             "pingpong/%s/fxcmd",  macAddress);
    snprintf(feedback_topic, sizeof(feedback_topic),
             "pingpong/%s/fxresp", macAddress);
    snprintf(debug_topic,    sizeof(debug_topic),
             "pingpong/%s/debug",  macAddress);

    snprintf(topic_available, sizeof(topic_available),
             "matrigs/0/sta/%s/available", station_name);

    snprintf(topic_station_state, sizeof(topic_station_state),
             "matrigs/0/sta/%s", station_name);

    snprintf(topic_dt, sizeof(topic_dt),          // ← correct legacy feed
             "matrigs/0/dt/RTX/d/%s", station_name);

    snprintf(topic_band_prefix, sizeof(topic_band_prefix),
             "matrigs/0/sta/%s/b/", station_name);
    snprintf(topic_band_wildcard, sizeof(topic_band_wildcard),
             "%s#", topic_band_prefix);

    /* ---------- console summary ----------------------------------- */
    Serial.printf("Pingpong broker: %s:%d\n", mqtt_server, mqtt_port);
    Serial.printf("MatriGS broker:  %s:%d\n", matrigs_server, matrigs_port);
    Serial.print("Command topic: ");  Serial.println(command_topic);
    Serial.print("Feedback topic: "); Serial.println(feedback_topic);
    Serial.print("Debug topic: ");    Serial.println(debug_topic);
    Serial.print("Sub → ");           Serial.println(topic_station_state);
    Serial.print("Sub → ");           Serial.println(topic_available);
    Serial.print("Sub → ");           Serial.println(topic_dt);
    Serial.print("Sub → ");           Serial.println(topic_band_wildcard);

#if defined(ARDUINO_ARCH_ESP32)
    if (MDNS.begin(station_name))
        Serial.println("[mDNS] responder started");
#endif
}


// --- AP Mode Setup ---
void setupAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);
  Serial.printf("AP SSID: %s\n", AP_SSID);
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());
}

// --- Load from EEPROM ---
void loadConfigFromEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(EEPROM_OFF_SSID, ssid);
  EEPROM.get(EEPROM_OFF_PASSWORD, password);
  EEPROM.get(EEPROM_OFF_MQTT_SERVER, mqtt_server);
  EEPROM.get(EEPROM_OFF_MQTT_PORT, mqtt_port);
  EEPROM.get(EEPROM_OFF_STATION, station_name);

  ssid[sizeof(ssid)-1]         = '\0';
  password[sizeof(password)-1] = '\0';
  mqtt_server[sizeof(mqtt_server)-1] = '\0';
  station_name[sizeof(station_name)-1] = '\0';

  // v2+ config fields (separate MatriGS broker) are guarded by a magic marker.
  uint32_t magic = 0;
  EEPROM.get(EEPROM_OFF_MAGIC, magic);
  if (magic == EEPROM_MAGIC_V2) {
    EEPROM.get(EEPROM_OFF_MATRIGS_SERVER, matrigs_server);
    EEPROM.get(EEPROM_OFF_MATRIGS_PORT, matrigs_port);
    matrigs_server[sizeof(matrigs_server) - 1] = '\0';
  } else {
    // Backward compat: old configs only had one broker.
    strncpy(matrigs_server, mqtt_server, sizeof(matrigs_server));
    matrigs_port = 4883;  // MatriGS default port in most installs
  }

  // Sanity defaults
  if (!matrigs_server[0] || (uint8_t)matrigs_server[0] == 0xFF)
    strncpy(matrigs_server, mqtt_server, sizeof(matrigs_server));
  if (matrigs_port <= 0 || matrigs_port > 65535)
    matrigs_port = 4883;

  EEPROM.end();

  Serial.println("Reading configuration from EEPROM");
  Serial.printf("Read SSID: %s\n", ssid);
  Serial.printf("Read Pingpong MQTT: %s:%d\n", mqtt_server, mqtt_port);
  Serial.printf("Read MatriGS MQTT:  %s:%d\n", matrigs_server, matrigs_port);
  Serial.printf("Read Station Name: %s\n", station_name);
}

// --- FX Command Parser ---
void processFxCommand(const char* msg) {
  int mode, brightness, speed;
  char colorStr[16];

  int parsed = sscanf(msg, "%d,%d,%d,%15s", &mode, &brightness, &speed, colorStr);
  if (parsed != 4) {
    publishDebugMessage("Standard command error: Error parsing message format. Parsed items: 0");
    return;
  }

  unsigned long color = strtoul(colorStr, NULL, 16);
  ws2812fx.setMode(mode);
  ws2812fx.setBrightness(brightness);
  ws2812fx.setSpeed(speed);
  ws2812fx.setColor(color);
  ws2812fx.start();

  publishDebugMessage("✅ FX command applied.");
}

// ────────────────────────────────────────────────────────────────
//  Parse and execute  segment,<pixel>,<mode>,<bright>,<speed>,<color>
//  Expects color as 6-digit hex (RRGGBB).  Uses the same validation
//  logic you had inline before.
// ────────────────────────────────────────────────────────────────
void processSegmentCommand(const char* msg)
{
  int segment, mode, brightness, speed;
  char colorStr[7] = {0};                      // 6 chars + NUL
  uint32_t color   = 0;

  int parsed = sscanf(msg, "segment,%d,%d,%d,%d,%6s",
                      &segment, &mode, &brightness, &speed, colorStr);

  if (parsed == 5 && segment >= 0 && segment < NUM_LEDS) {

      color = strtoul(colorStr, nullptr, 16);

      if (brightness < 0 || brightness > 255 ||
          mode < 0 || mode >= ws2812fx.getModeCount() ||
          speed < 0) {

          publishDebugMessage("[Segment] Invalid parameters");
          return;
      }

      ws2812fx.setSegment(segment, segment, segment,
                          mode, color, speed, NO_OPTIONS);
      ws2812fx.setBrightness(brightness);
      ws2812fx.start();

      publishDebugMessage("[Segment] Command executed");
  }
  else {
      publishDebugMessage("[Segment] Parse error");
  }
}



// ────────────────────────────────────────────────────────────────
//  MQTT callback
//  • Exact-match dispatch using pre-built topic strings
//  • Bullet-proof payload copy
//  • Handles:
//        – …/available    → handleAvailableJSON()
//        – …/dt/…         → handleCurrentBandJSON()
//        – …/fxcmd        → stop / segment / standard FX string
//  • All debug goes out *after* dispatch so handlers can safely
//    publish to MQTT without recursion
// ────────────────────────────────────────────────────────────────
/* ================================================================
 *  PubSubClient message callback
 * ===============================================================*/
void callback(char* topic, byte* payload, unsigned int length)
{
    /* ---------- copy MQTT payload into NUL-terminated buffer ------ */
    static const size_t MQTT_BUF_LEN = 2048;          // ≈2 kB
    static char msg[MQTT_BUF_LEN];

    if (length >= MQTT_BUF_LEN) {                     // still too big
        Serial.printf("[MQTT] ⚠ oversized payload (%u bytes) – ignored\n",
                      length);
        return;
    }
    memcpy(msg, payload, length);
    msg[length] = '\0';

    /* ---------- make a writable copy of the topic string ---------- */
    char topicCopy[160];
    strncpy(topicCopy, topic, sizeof(topicCopy) - 1);
    topicCopy[sizeof(topicCopy) - 1] = '\0';

    bool handled = false;                             // fall-through flag

    /* --------------------------------------------------------------
     *  1. “…/sta/<station>”
     * -------------------------------------------------------------*/
    if (strcmp(topicCopy, topic_station_state) == 0) {
        handleStationStateJSON(msg);
        handled = true;
    }

    /* --------------------------------------------------------------
     *  2. “…/sta/<station>/available”
     * -------------------------------------------------------------*/
    else if (strcmp(topicCopy, topic_available) == 0) {
        handleAvailableJSON(msg);
        handled = true;
    }

    /* --------------------------------------------------------------
     *  3. legacy “…/dt/RTX/d/<station>”
     * -------------------------------------------------------------*/
    else if (strcmp(topicCopy, topic_dt) == 0) {
        handleCurrentBandJSON(msg);
        handled = true;
    }

    /* --------------------------------------------------------------
     *  4. per-band live JSON “…/sta/<station>/b/<Band>”
     * -------------------------------------------------------------*/
    else if (strncmp(topicCopy, topic_band_prefix,
                     strlen(topic_band_prefix)) == 0) {

        const char* bandTok = topicCopy + strlen(topic_band_prefix);  // "B80…"
        const char* slash   = strchr(bandTok, '/');

        if (!slash) {                          // base topic only
            handleBandStateJSON(bandTok, msg);
            handled = true;
        }
    }

    /* --------------------------------------------------------------
     *  5. LED FX commands  pingpong/<MAC>/fxcmd
     * -------------------------------------------------------------*/
    else if (strcmp(topicCopy, command_topic) == 0) {

      // Utility command to (re-)enable debug without a serial cable.
      // Publish "debug" to pingpong/<MAC>/fxcmd.
      if (strcmp(msg, "debug") == 0 || strcmp(msg, "DEBUG") == 0) {
          enableDebugFor1Minute("MQTT");
          handled = true;
          return;
      }

      if (debugEnabled) {                    // ① don’t always print/publish
          publishDebugMessage("[FX] cmd RX");
      }

        if (strcmp(msg, "stop") == 0) {                    // simple stop
            ws2812fx.stop();
            ws2812fx.clear();
            publishDebugMessage("[FX] 📴 stop");
        }
        else if (strncmp(msg, "segment,", 8) == 0) {       // advanced
            processSegmentCommand(msg);
        }
        else {                                             // standard
            processFxCommand(msg);
        }
        ws2812fx.service(); 
        handled = true;
    }

    /* --------------------------------------------------------------
     *  6. unknown / unhandled topic
     * -------------------------------------------------------------*/
    if (!handled) {
        char dbg[192];
        snprintf(dbg, sizeof(dbg), "[MQTT] ⏭ ignored topic %s", topicCopy);
        publishDebugMessage(dbg);
    }
}

// --- MQTT reconnect (Pingpong broker: LED commands + debug) ---
static void reconnectMqttCmd()
{
  if (clientCmd.connected()) return;

  Serial.println("Attempting MQTT Pingpong connection...");
  char clientId[64];
  snprintf(clientId, sizeof(clientId), "PingPong-CMD-%s", macAddress);

  if (!clientCmd.connect(clientId)) return;

  publishDebugMessage("[MQTT] Pingpong connected");

  /* ---------- 5-second rainbow hello ------------------------- */
  ws2812fx.setMode(11);                // FX_MODE_RAINBOW_CYCLE
  ws2812fx.setBrightness(108);
  ws2812fx.setSpeed(200);
  ws2812fx.setColor(0xFF0000);         // colour arg unused by mode 11
  ws2812fx.start();

  mqttHelloRunning = true;
  mqttHelloStart   = millis();

  clientCmd.subscribe(command_topic);

  // Keypad status is useful even when debug is disabled, so publish it
  // directly to the debug topic after MQTT connects.
  if (debug_topic[0]) {
    char kmsg[96];
    if (g_keypadPresent)
      snprintf(kmsg, sizeof(kmsg), "[Keypad] present @0x%02X (SDA=D2 SCL=D1 3.3V)", g_keypadI2cAddr);
    else
      snprintf(kmsg, sizeof(kmsg), "[Keypad] absent (expected 0x20..0x27, power=3.3V)");
    clientCmd.publish(debug_topic, kmsg);
  }

  snprintf(logBuffer, sizeof(logBuffer), "Hello %s", VER);
  if (feedback_topic[0])
    clientCmd.publish(feedback_topic, logBuffer);
}

// --- MQTT reconnect (MatriGS broker: station state + antenna switching) ---
static void reconnectMqttMatrigs()
{
  if (clientMatrigs.connected()) return;

  Serial.println("Attempting MQTT MatriGS connection...");
  char clientId[64];
  snprintf(clientId, sizeof(clientId), "PingPong-MGS-%s", macAddress);

  if (!clientMatrigs.connect(clientId)) return;

  publishDebugMessage("[MQTT] MatriGS connected");

  clientMatrigs.subscribe(topic_station_state);
  clientMatrigs.subscribe(topic_available);
  clientMatrigs.subscribe(topic_dt);
  clientMatrigs.subscribe(topic_band_wildcard);
}

/* ================================================================
 *  Shared actions (Serial and external keypad)
 * ===============================================================*/
static void enableDebugFor1Minute(const char* source)
{
    debugEnabled  = true;
    debugDeadline = millis() + 60'000;    // auto-off in 1 min
    Serial.println(F("🟢 Debug enabled for 1 minute"));

    char msg[128];
    snprintf(msg, sizeof(msg), "[%s] debug ON (1 min)", source);
    publishDebugMessage(msg);
}

static void stopLedEffects(const char* source)
{
    ws2812fx.stop();
    ws2812fx.clear();
    Serial.println(F("[LED] stopped"));

    char msg[128];
    snprintf(msg, sizeof(msg), "[%s] LEDs stopped", source);
    publishDebugMessage(msg);
}

static void refreshAntennaListForCurrentBandIfNeeded()
{
    // When MatriGS doesn't provide a "current band" (eg OFFLINE),
    // `currentAntList` may be empty even though `/available` has data.
    // Refresh lazily from the availability cache based on `currentBand`.
    static String lastBand = "";
    if (!currentBand.length()) return;
    if (currentBand == "?" || currentBand == "-") return;
    if (currentBand == lastBand && !currentAntList.empty()) return;

    currentAntennas = listAntennasForBand(currentBand.c_str());
    currentAntList  = getCurrentAntList();
    lastBand        = currentBand;
}

// idx:
// - 0     => LOAD-2KA dummy load
// - 1..N  => currentAntList[idx-1]
static void selectAntennaByIndex(int idx, const char* source)
{
    if (idx < 0) return;

    void (*pub)(const char*) = publishDebugMessage;
    if (source && strcmp(source, "Keypad") == 0)
        pub = publishDebugMessageAlways;

    refreshAntennaListForCurrentBandIfNeeded();

    /* map index to antenna name -------------------------------- */
    String chosen;
    if (idx == 0) {
        chosen = F("LOAD-2KA");               // dummy load
    } else if (idx <= (int)currentAntList.size()) {
        chosen = currentAntList[idx - 1];     // 1-based → 0-based
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "[%sSelect] Invalid antenna index %d (max %d) band=%s",
                 source, idx, (int)currentAntList.size(), currentBand.c_str());
        pub(msg);
        return;
    }

    char msg[192];
    snprintf(msg, sizeof(msg), "[%sSelect] %s band=%s -> %s",
             source, currentRXTX, currentBand.c_str(), chosen.c_str());
    pub(msg);

    /* find currently active antenna in this bank --------------- */
    BandState curState   = g_bandStates[currentBand];
    String    currentSel = (currentRXTX[0] == 'R') ? curState.rx
                                                   : curState.tx;

    /* 1️⃣ TX bank & PTT active  →  queue change ---------------- */
    if (currentRXTX[0] == 'T' && g_txActive) {
        g_pendingTx.band   = currentBand;
        g_pendingTx.oldAnt = currentSel;
        g_pendingTx.newAnt = chosen;
        g_pendingTx.valid  = true;

        snprintf(msg, sizeof(msg), "[TX-Queue] queued via %s until RTX returns to RX", source);
        pub(msg);
        return;
    }

    /* 2️⃣ publish immediately (RX, or TX while in RX) ---------- */
    if (!clientMatrigs.connected()) {
        snprintf(msg, sizeof(msg), "[%sSelect] MatriGS MQTT not connected", source);
        pub(msg);
    } else if (currentSel == chosen) {
        snprintf(msg, sizeof(msg), "[%sSelect] already active", source);
        pub(msg);
    } else {
        /* remove old (if any) */
        if (currentSel.length()) {
            char topicR[128];
            snprintf(topicR, sizeof(topicR),
                     "matrigs/0/sta/%s/b/%s/p:remove/%sANTENNAS",
                     station_name, currentBand.c_str(), currentRXTX);
            clientMatrigs.publish(topicR, currentSel.c_str());
        }
        /* add new */
        char topicA[128];
        snprintf(topicA, sizeof(topicA),
                 "matrigs/0/sta/%s/b/%s/p:add/%sANTENNAS",
                 station_name, currentBand.c_str(), currentRXTX);
        clientMatrigs.publish(topicA, chosen.c_str());
    }

    /* ----------------------------------------------------------
     *  3️⃣ update cache -----------------------------------------
     * ---------------------------------------------------------*/
    if (currentRXTX[0] == 'R')
        g_bandStates[currentBand].rx = chosen;
    else
        g_bandStates[currentBand].tx = chosen;
}

static bool parseUnsignedInt(const String& s, int* out)
{
    if (!out) return false;
    if (!s.length()) return false;

    int v = 0;
    for (size_t i = 0; i < s.length(); ++i) {
        if (!isDigit(s[i])) return false;
        v = (v * 10) + (s[i] - '0');
        if (v > 1000) break;  // sanity cap
    }
    *out = v;
    return true;
}

/* ================================================================
 *  External keypad support (Dongutec 4x4 via MCP23008)
 *  Key index 0..15 is translated to a keypad label for mapping.
 * ===============================================================*/
static char keypadLabelFromKeyIndex(int8_t keyIndex)
{
    // Default mapping for a typical 4x4 keypad:
    //  [ 1 2 3 A ]
    //  [ 4 5 6 B ]
    //  [ 7 8 9 C ]
    //  [ * 0 # D ]
    // The MCP23008 example returns keys as col*4 + row (column-major).
    static const char labels[16] = {
        '1','4','7','*',
        '2','5','8','0',
        '3','6','9','#',
        'A','B','C','D',
    };
    if (keyIndex < 0 || keyIndex > 15) return '?';
    return labels[(uint8_t)keyIndex];
}

static int keypadSelectionIndexFromLabel(char label)
{
    if (label >= '0' && label <= '9') return label - '0';
    switch (label) {
      case 'A': return 10;
      case 'B': return 11;
      case 'C': return 12;
      case 'D': return 13;
      case '*': return 14;
      case '#': return 15;
      default:  return -1;
    }
}

static void handleKeypad()
{
    if (!g_keypadPresent) return;

    static uint32_t lastPoll = 0;
    static int8_t   candidateKey = -1;
    static uint32_t candidateSince = 0;
    static int8_t   debouncedKey = -1;
    static bool     multiActive = false;

    const uint32_t now = millis();
    if (now - lastPoll < KEYPAD_POLL_MS) return;
    lastPoll = now;

    const int8_t rawKey = g_keypad.readKeyIndex();
    if (rawKey == -2) {
        static uint32_t lastErr = 0;
        if (debugEnabled && now - lastErr > 2000) {
            publishDebugMessage("[Keypad] I2C error (check wiring/address)");
            lastErr = now;
        }
        return;
    }

    if (rawKey == -3) {
        if (!multiActive) {
            publishDebugMessageAlways("[Keypad] multi-key press detected (ignored)");
            multiActive = true;
        }
        // Cancel any in-progress debounce while multiple keys are held.
        candidateKey = -1;
        debouncedKey = -1;
        candidateSince = now;
        return;
    }
    multiActive = false;

    if (rawKey != candidateKey) {
        candidateKey = rawKey;
        candidateSince = now;
    }

    if (now - candidateSince < KEYPAD_DEBOUNCE_MS) return;
    if (debouncedKey == candidateKey) return;  // no state change

    debouncedKey = candidateKey;

    // Only act on press transitions (ignore releases).
    if (debouncedKey < 0) return;

    const char label = keypadLabelFromKeyIndex(debouncedKey);
    const int  idx   = keypadSelectionIndexFromLabel(label);

    {
        // Key presses are user-triggered, so always publish even if debug timed out.
        char msg[96];
        snprintf(msg, sizeof(msg), "[Keypad] raw=%d label=%c idx=%d", debouncedKey, label, idx);
        publishDebugMessageAlways(msg);
    }

    if (idx >= 0) {
        selectAntennaByIndex(idx, "Keypad");
    }
}

/* ================================================================
 *  Read one line from USB-Serial and act on it.
 *  • utility text commands:  debug, stop
 *  • quick-select number (0..N) → antenna switch / queue
 * ===============================================================*/
void handleSerialCommands()
{
    /* ── nothing waiting? ────────────────────────────────────── */
    if (!Serial.available()) return;

    /* ── read one LF-terminated line and trim whitespace ─────── */
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (!cmd.length()) return;                // empty line

    /* ----------------------------------------------------------
     *  textual utility commands
     * ---------------------------------------------------------*/
    if (cmd.equalsIgnoreCase("debug")) {
        enableDebugFor1Minute("Serial");
        return;
    }

    if (cmd.equalsIgnoreCase("stop")) {
        stopLedEffects("Serial");
        return;
    }

    /* ----------------------------------------------------------
     *  quick-select: unsigned integer (eg "0", "1", ... "15")
     * ---------------------------------------------------------*/
    int idx = -1;
    if (!parseUnsignedInt(cmd, &idx)) {
        Serial.println(F("[Serial] Unrecognised command - try: debug, stop, or a number (0..15)"));
        return;
    }

    selectAntennaByIndex(idx, "Serial");
}




void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("Booting...");
  Serial.println(VER);
  debugDeadline += millis();          // absolute timestamp, ~60 s from now
  Serial.println(F("🟢 Debug enabled for 1 minute after boot"));

  // I2C keypad (optional)
  Wire.begin(KEYPAD_SDA_PIN, KEYPAD_SCL_PIN);
  Wire.setClock(100000);
  g_keypadPresent = false;
  g_keypadI2cAddr = 0;

  // Probe the full MCP23008 address range (0x20..0x27) so we don't
  // depend on how the address pads are configured on the board.
  for (uint8_t addr = 0x20; addr <= 0x27; ++addr) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      g_keypadI2cAddr = addr;
      break;
    }
  }

  if (g_keypadI2cAddr) {
    g_keypad = DongutecKeypadMcp23008(g_keypadI2cAddr);
    g_keypadPresent = g_keypad.begin(Wire);
  }

  if (g_keypadPresent) {
    char msg[64];
    snprintf(msg, sizeof(msg), "[Keypad] MCP23008 detected @0x%02X", g_keypadI2cAddr);
    publishDebugMessage(msg);
  } else {
    publishDebugMessage("[Keypad] MCP23008 not detected (0x20..0x27)");
  }

  loadConfigFromEEPROM();
  setupWiFi();

  server.on("/", handleRoot);
  server.on("/save", handleSave);
  server.on("/update", HTTP_GET, handleUpdate);
  httpUpdater.setup(&server);
  server.begin();
  Serial.println("HTTP server started");

  ws2812fx.init();
  ws2812fx.setBrightness(100);
  ws2812fx.setSpeed(500);
  ws2812fx.setColor(0x0000FF);
  ws2812fx.setMode(FX_MODE_STATIC);
  ws2812fx.start();

  // MQTT Pingpong broker (LED commands + debug)
  clientCmd.setBufferSize(512);
  clientCmd.setServer(mqtt_server, mqtt_port);
  clientCmd.setCallback(callback);

  // MQTT MatriGS broker (station state + antenna switching)
  clientMatrigs.setBufferSize(2048);
  clientMatrigs.setServer(matrigs_server, matrigs_port);
  clientMatrigs.setCallback(callback);
}

void loop() {
  handleSerialCommands();   // react to "debug" / "stop" over USB
  handleKeypad();           // external 4x4 keypad via MCP23008 (optional)
  // --------------------------------------------------------------
  //  Auto-disable debug once the first minute has passed
  // --------------------------------------------------------------
  if (debugEnabled && debugDeadline && millis() >= debugDeadline) {
      debugEnabled   = false;
      debugDeadline  = 0;             // stop checking
      Serial.println(F("🔴 Debug messages DISABLED (1-minute timeout)"));
  }

  static unsigned long lastMQTTCheck = 0;
  unsigned long now = millis();

  ws2812fx.service();
  server.handleClient();
  clientCmd.loop();
  clientMatrigs.loop();

  if (mqttHelloRunning && millis() - mqttHelloStart >= 5000) {
      ws2812fx.stop();
      ws2812fx.clear();        // blank the strip
      mqttHelloRunning = false;
  }


  if (WiFi.status() != WL_CONNECTED) {
    if (WiFi.getMode() != WIFI_AP) {
      publishDebugMessage("Loop: WiFi disconnected. Switching to AP mode.");
      ws2812fx.setMode(FX_MODE_BLINK);
      ws2812fx.setColor(0xFF0000);
      ws2812fx.start();
    }
    if (clientCmd.connected() || clientMatrigs.connected()) {
      publishDebugMessage("Loop: WiFi lost, disconnecting MQTT.");
      if (clientCmd.connected()) clientCmd.disconnect();
      if (clientMatrigs.connected()) clientMatrigs.disconnect();
    }
  } else {
    if (WiFi.getMode() != WIFI_STA) {
      publishDebugMessage("Loop: WiFi connected! Switching from AP to STA indication.");
      WiFi.mode(WIFI_STA);
      ws2812fx.stop();
      ws2812fx.clear();
    }
  }

  if (WiFi.status() == WL_CONNECTED && (now - lastMQTTCheck > 5000)) {
    lastMQTTCheck = now;
    reconnectMqttCmd();
    reconnectMqttMatrigs();
  }
}
