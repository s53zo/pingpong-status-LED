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

#include "antenna_mqtt_handler.h"

// WiFi + MQTT setup
WiFiClient espClient;
PubSubClient client(espClient);
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

// Constants
#define VER "v1.39-debug jul 2025 S53ZO"
#define LED_PIN D3
#define NUM_LEDS 4
#define AP_SSID "ESP8266_Setup"
WS2812FX ws2812fx = WS2812FX(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// Buffers
char ssid[32], password[32], mqtt_server[40], station_name[32];
int mqtt_port = 1883;
char logBuffer[256];

char macAddress[18]     = "";          
char command_topic[64]  = "";
char feedback_topic[64] = "";
char debug_topic[64]    = "";
char currentRXTX[4] = "RX";     

bool        mqttHelloRunning = false;
unsigned long mqttHelloStart = 0;



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
char topic_available[128];
char topic_dt[128];


// -----------------------------------------------------------------
//  Debug helper – only speaks when debugEnabled == true
// -----------------------------------------------------------------
void publishDebugMessage(const char* msg) {
  if (!debugEnabled) return;              // ← early exit

  Serial.println(msg);

  if (client.connected()) {
    char debugTopic[100];
    snprintf(debugTopic, sizeof(debugTopic),
             "pingpong/%s/debug", WiFi.macAddress().c_str());
    client.publish(debugTopic, msg);
  }
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

/* -------- handleRoot() rewritten ---------------------------------- */
void handleRoot() {
  publishDebugMessage("Handling root page request");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");        // headers first

  send_P(PAGE_HEAD);

  /* ---------- form ---------- */
  send_P(PAGE_FORM1); server.sendContent(ssid);
  send_P(PAGE_FORM2); server.sendContent(password);
  send_P(PAGE_FORM3); server.sendContent(mqtt_server);
  send_P(PAGE_FORM4); server.sendContent(String(mqtt_port));
  send_P(PAGE_FORM5); server.sendContent(station_name);
  send_P(PAGE_FORM6);

  /* ---------- status list ---------- */
  send_P(PAGE_STATUS_HEAD);
  server.sendContent("<li>Firmware: <b>" + String(VER) + "</b></li>");
  server.sendContent("<li>MAC Address: <code>" + String(macAddress) + "</code></li>");
  server.sendContent("<li>Station: <b>" + String(station_name) + "</b></li>");
  server.sendContent("<li>Current band: " + currentBand + "</li>");
  server.sendContent("<li>Antennas for band: " + currentAntennas + "</li>");
  server.sendContent("<li>MQTT command topic: <code>" + String(command_topic) + "</code></li>");
  server.sendContent("<li>MQTT feedback topic: <code>" + String(feedback_topic) + "</code></li>");
  server.sendContent("<li>Debug topic: <code>" + String(debug_topic) + "</code></li>");
  send_P(PAGE_CHEAT);              // static head
  server.sendContent(command_topic);   // live topic
  send_P(PAGE_CHEAT_TAIL);         // list + link
  send_P(PAGE_FOOT);
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
  strncpy(station_name, server.arg("station_name").c_str(), sizeof(station_name));

  EEPROM.begin(512);
  EEPROM.put(0, ssid);
  EEPROM.put(32, password);
  EEPROM.put(64, mqtt_server);
  EEPROM.put(104, mqtt_port);
  EEPROM.put(108, station_name);
  EEPROM.commit();
  EEPROM.end();

  server.send(200, "text/html", "<html><body><h2>Saved. Rebooting...</h2></body></html>");
  delay(1000);
  ESP.restart();
}

// --- WiFi Setup ---
void setupWiFi() {
  WiFi.mode(WIFI_STA);            // station only
  WiFi.begin(ssid, password);
  Serial.printf("Connecting to WiFi: %s …\n", ssid);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { // 15 s
    delay(100);
    yield();                     // feed watchdog
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected! IP = ");
    String macStr = WiFi.macAddress();                 // "AA:BB:CC:DD:EE:FF"
    macStr.toCharArray(macAddress, sizeof(macAddress));

    snprintf(command_topic,  sizeof(command_topic),
            "pingpong/%s/fxcmd",  macAddress);
    snprintf(feedback_topic, sizeof(feedback_topic),
            "pingpong/%s/fxresp", macAddress);
    snprintf(debug_topic,    sizeof(debug_topic),
            "pingpong/%s/debug",  macAddress);
    snprintf(topic_cmd,       sizeof(topic_cmd), "pingpong/%s/fxcmd",   macAddress);
    snprintf(topic_available, sizeof(topic_available),
         "matrigs/0/sta/%s/available", station_name);
    snprintf(topic_dt,        sizeof(topic_dt),
         "matrigs/0/dt/RTX/d/%s",      station_name);


  Serial.print("Command topic: ");  Serial.println(command_topic);
  Serial.print("Feedback topic: "); Serial.println(feedback_topic);
  Serial.print("Debug topic: ");    Serial.println(debug_topic);
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Timeout – starting AP fallback");
    setupAP();                   // creates ESP8266_Setup if STA failed
  }
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
  EEPROM.begin(512);
  EEPROM.get(0, ssid);
  EEPROM.get(32, password);
  EEPROM.get(64, mqtt_server);
  EEPROM.get(104, mqtt_port);
  EEPROM.get(108, station_name);

  ssid[sizeof(ssid)-1]         = '\0';
  password[sizeof(password)-1] = '\0';
  mqtt_server[sizeof(mqtt_server)-1] = '\0';
  station_name[sizeof(station_name)-1] = '\0';

  EEPROM.end();

  Serial.println("Reading configuration from EEPROM");
  Serial.printf("Read SSID: %s\n", ssid);
  Serial.printf("Read MQTT Server: %s\n", mqtt_server);
  Serial.printf("Read MQTT Port: %d\n", mqtt_port);
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
void callback(char* topic, byte* payload, unsigned int length)
{
  /* ── 1️⃣  Safe copies (topic & payload) -------------------------- */
  char topicCopy[128];
  strncpy(topicCopy, topic, sizeof(topicCopy) - 1);
  topicCopy[sizeof(topicCopy) - 1] = '\0';

  /* guard: payload ≤ MQTT_MAX_PACKET_SIZE */
  if (length >= MQTT_MAX_PACKET_SIZE) {
      publishDebugMessage("[callback] ❌ Payload too big");
      return;
  }
  char* msg = static_cast<char*>(malloc(length + 1));
  if (!msg) { publishDebugMessage("[callback] ❌ malloc failed"); return; }
  memcpy(msg, payload, length);
  msg[length] = '\0';

  bool handled = false;

  /* ── 2️⃣  Exact-match topic dispatch ------------------------------ */
  extern char topic_available[];
  extern char topic_dt[];
  extern char topic_cmd[];

  if (strcmp(topicCopy, topic_available) == 0) {
      handleAvailableJSON(msg);
      handled = true;
  }
  else if (strcmp(topicCopy, topic_dt) == 0) {
      handleCurrentBandJSON(msg);
      handled = true;
  }
  else if (strcmp(topicCopy, topic_cmd) == 0) {
      /* ---- fxcmd sub-protocol ----------------------------------- */
      if (strcmp(msg, "stop") == 0) {
          ws2812fx.stop(); ws2812fx.clear();
          publishDebugMessage("[FX] stop");
      }
      else if (strncmp(msg, "segment", 7) == 0) {
          // reuse your existing segment-parsing block
          processSegmentCommand(msg);          
      }
      else if (strchr(msg, ',') && isdigit((uint8_t)msg[0])) {
          processFxCommand(msg);
      }
      else {
          publishDebugMessage("[FX] unrecognised payload");
      }
      handled = true;
  }

  /* ── 3️⃣  Post-dispatch debug (safe to publish) ------------------- */
  snprintf(logBuffer, sizeof(logBuffer), "Topic: %s", topicCopy);
  publishDebugMessage(logBuffer);
  snprintf(logBuffer, sizeof(logBuffer),
           "MQTT Payload: '%s'%s",
           msg, handled ? "" : "  (UNHANDLED)");
  publishDebugMessage(logBuffer);

  free(msg);
}



// --- MQTT reconnect ---
void reconnectMQTT() {
  if (!client.connected()) {
    Serial.println("Attempting MQTT connection...");
    char clientId[64];
    snprintf(clientId, sizeof(clientId), "PingPong-%s", WiFi.macAddress().c_str());

    if (client.connect(clientId)) {
      publishDebugMessage("MQTT connected successfully.");

      /* ---------- 5-second rainbow hello ------------------------- */
      ws2812fx.setMode(11);                // FX_MODE_RAINBOW_CYCLE
      ws2812fx.setBrightness(108);
      ws2812fx.setSpeed(200);
      ws2812fx.setColor(0xFF0000);         // colour arg unused by mode 11
      ws2812fx.start();

      mqttHelloRunning = true;
      mqttHelloStart   = millis();


      char topic_cmd[128], topic1[128], topic2[128];
      strncpy(topic_cmd, command_topic, sizeof(topic_cmd));
      snprintf(topic1, sizeof(topic1), "matrigs/0/sta/%s/available", station_name);
      snprintf(topic2, sizeof(topic2), "matrigs/0/dt/RTX/d/%s", station_name);

      client.subscribe(topic_cmd);
      client.subscribe(topic1);
      client.subscribe(topic2);

      snprintf(logBuffer, sizeof(logBuffer), "Subscribed to command topic: %s", topic_cmd);
      publishDebugMessage(logBuffer);
      snprintf(logBuffer, sizeof(logBuffer), "Subscribed to: %s and %s", topic1, topic2);
      publishDebugMessage(logBuffer);

      snprintf(logBuffer, sizeof(logBuffer), "Hello %s", VER);
      char feedbackTopic[100];
      snprintf(feedbackTopic, sizeof(feedbackTopic), "pingpong/%s/fxresp", WiFi.macAddress().c_str());
      client.publish(feedbackTopic, logBuffer);
    }
  }
}

void handleSerialCommands()
{
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();                                   // remove CR/LF

  /* ── debug on/off ───────────────────────────────────────────── */
  if (cmd.equalsIgnoreCase("debug")) {
      debugEnabled  = true;
      debugDeadline = 0;
      Serial.println(F("🟢 Debug messages ENABLED"));
      return;
  }
  if (cmd.equalsIgnoreCase("stop")) {
      debugEnabled  = false;
      debugDeadline = 0;
      Serial.println(F("🔴 Debug messages DISABLED"));
      return;
  }

  /* ── antenna quick-select (single digit) ───────────────────── */
  if (cmd.length() == 1 && isDigit(cmd[0])) {

      /* robust parse: distinguish “0” from invalid */
      char* endPtr;
      long  v = strtol(cmd.c_str(), &endPtr, 10);
      if (*endPtr != '\0' || v < 0 || v > 9) {
          Serial.println(F("[Serial] Invalid number"));
          return;
      }
      int idx = (int)v;

      extern std::vector<String> currentAntList;
      extern PubSubClient        client;
      extern char                station_name[];
      extern String              currentBand;
      extern char                currentRXTX[];

      String chosen;
      if (idx == 0) {                             // always LOAD-2KA
          chosen = F("LOAD-2KA");
      } else if (idx <= (int)currentAntList.size()) {
          String candidate = currentAntList[idx - 1];
          if (candidate != F("LOAD-2KA"))         // prevent alias
              chosen = candidate;
      }

      if (chosen.length()) {
          char buf[160];
          snprintf(buf, sizeof(buf),
                   "[SerialSelect] Antenna %d → %s", idx, chosen.c_str());
          Serial.println(buf);
          publishDebugMessage(buf);

          /* publish to set/TXANTENNAS or set/RXANTENNAS */
          if (client.connected()) {
              char topic[128];
              snprintf(topic, sizeof(topic),
                       "matrigs/0/sta/%s/b/%s/p:set/%sANTENNAS",
                       station_name, currentBand.c_str(), currentRXTX);

              char payload[80];
              snprintf(payload, sizeof(payload), "[\"%s\"]", chosen.c_str());

              client.publish(topic, payload);
          }

          // TODO: add GPIO / relay switching here

      } else {
          Serial.println(F("[SerialSelect] Invalid antenna number"));
      }
      return;
  }

  /* ── everything else ───────────────────────────────────────── */
  Serial.println(F("[Serial] Unknown command"));
}



void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("Booting...");
  Serial.println(VER);
  debugDeadline += millis();          // absolute timestamp, ~60 s from now
  Serial.println(F("🟢 Debug enabled for 1 minute after boot"));

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

  client.setBufferSize(2048);
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

void loop() {
  handleSerialCommands();   // react to "debug" / "stop" over USB
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
  client.loop();

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
    if (client.connected()) {
      publishDebugMessage("Loop: WiFi lost, disconnecting MQTT.");
      client.disconnect();
    }
  } else {
    if (WiFi.getMode() != WIFI_STA) {
      publishDebugMessage("Loop: WiFi connected! Switching from AP to STA indication.");
      WiFi.mode(WIFI_STA);
      ws2812fx.stop();
      ws2812fx.clear();
    }
  }

  if (!client.connected() && (now - lastMQTTCheck > 5000)) {
    lastMQTTCheck = now;
    reconnectMQTT();
  }
}
