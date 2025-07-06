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
/* ================================================================
 *  Bring up Wi-Fi, then build all MQTT topics & subscribe
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

    snprintf(topic_dt, sizeof(topic_dt),          // ← correct legacy feed
             "matrigs/0/dt/RTX/d/%s", station_name);

    snprintf(topic_band_prefix, sizeof(topic_band_prefix),
             "matrigs/0/sta/%s/b/", station_name);
    snprintf(topic_band_wildcard, sizeof(topic_band_wildcard),
             "%s#", topic_band_prefix);

    /* ---------- subscribe ----------------------------------------- */
    client.subscribe(command_topic);              // if broker echoes cmds
    client.subscribe(topic_available);            // antenna catalogue
    client.subscribe(topic_dt);                   // legacy combined status
    client.subscribe(topic_band_wildcard);        // live per-band JSON

    /* ---------- console summary ----------------------------------- */
    Serial.print("Command topic: ");  Serial.println(command_topic);
    Serial.print("Feedback topic: "); Serial.println(feedback_topic);
    Serial.print("Debug topic: ");    Serial.println(debug_topic);
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
     *  1. “…/sta/<station>/available”
     * -------------------------------------------------------------*/
    if (strcmp(topicCopy, topic_available) == 0) {
        handleAvailableJSON(msg);
        handled = true;
    }

    /* --------------------------------------------------------------
     *  2. legacy “…/dt/RTX/d/<station>”
     * -------------------------------------------------------------*/
    else if (strcmp(topicCopy, topic_dt) == 0) {
        handleCurrentBandJSON(msg);
        handled = true;
    }

    /* --------------------------------------------------------------
     *  3. per-band live JSON “…/sta/<station>/b/<Band>”
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
     *  4. LED FX commands  pingpong/<MAC>/fxcmd
     * -------------------------------------------------------------*/
    else if (strcmp(topicCopy, command_topic) == 0) {

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
        handled = true;
    }

    /* --------------------------------------------------------------
     *  5. unknown / unhandled topic
     * -------------------------------------------------------------*/
    if (!handled) {
        char dbg[192];
        snprintf(dbg, sizeof(dbg), "[MQTT] ⏭ ignored topic %s", topicCopy);
        publishDebugMessage(dbg);
    }
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

/* ================================================================
 *  Read one line from USB-Serial and act on it.
 *
 *  • User types a single digit 0-9 + <Enter>.
 *      0  →  hard-wired dummy load  ("LOAD-2KA")
 *      1… →  entry n-1 of currentAntList (band-sorted list)
 *
 *  • If the target bank is RX  →  publish immediately
 *    If the target bank is TX and PTT is active (g_txActive == true)
 *      →  queue the change in g_pendingTx and wait until RTX returns to RX
 *
 *  • Publishes     p:remove/…ANTENNAS   (plain text old antenna)
 *                  p:add/…ANTENNAS      (plain text new antenna)
 * ===============================================================*/
void handleSerialCommands()
{
    /* ── nothing waiting? ────────────────────────────────────── */
    if (!Serial.available()) return;

    /* ── read one LF-terminated line and trim whitespace ─────── */
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (!cmd.length()) return;

    /* ── quick-select: exactly one decimal digit 0-9 ─────────── */
    if (cmd.length() != 1 || !isDigit(cmd[0])) {
        Serial.println(F("[Serial] ❓ Unrecognised command – try a digit 0-9"));
        return;
    }

    int idx = cmd[0] - '0';

    /* ── map digit to antenna name ───────────────────────────── */
    String chosen;
    if (idx == 0) {                                    // dummy-load shortcut
        chosen = F("LOAD-2KA");
    } else if (idx <= (int)currentAntList.size()) {
        chosen = currentAntList[idx - 1];              // 1-based → 0-based
    } else {
        Serial.printf("[SerialSelect] Invalid antenna number %d\n", idx);
        return;
    }

    Serial.printf("[SerialSelect] %s band=%s → %s\n",
                  currentRXTX, currentBand.c_str(), chosen.c_str());

    /* ── figure out what is currently active in this bank ───── */
    BandState  curState   = g_bandStates[currentBand];
    String     currentSel = (currentRXTX[0] == 'R') ? curState.rx
                                                    : curState.tx;

    /* ── 1.  TX bank + PTT active  →  queue the change ───────── */
    if (currentRXTX[0] == 'T' && g_txActive) {
        g_pendingTx.band   = currentBand;
        g_pendingTx.oldAnt = currentSel;
        g_pendingTx.newAnt = chosen;
        g_pendingTx.valid  = true;

        publishDebugMessage("[TX-Queue] 💤 queued until RTX returns to RX");
        return;                                         // nothing published now
    }

    /* ── 2.  publish immediately (RX bank, or TX while in RX) ─ */
    if (!client.connected()) {
        publishDebugMessage("[SerialSelect] ⚠ MQTT not connected");
    } else if (currentSel == chosen) {
        publishDebugMessage("[SerialSelect] 🔄 already active");
    } else {
        
        /* 2️⃣ add new ----------------------------------------- */
        char topicA[128];
        snprintf(topicA, sizeof(topicA),
                 "matrigs/0/sta/%s/b/%s/p:add/%sANTENNAS",
                 station_name, currentBand.c_str(), currentRXTX);
        client.publish(topicA, chosen.c_str());

        /* 1️⃣ remove current (if any) -------------------------- */
        if (currentSel.length()) {
            char topicR[128];
            snprintf(topicR, sizeof(topicR),
                     "matrigs/0/sta/%s/b/%s/p:remove/%sANTENNAS",
                     station_name, currentBand.c_str(), currentRXTX);
            client.publish(topicR, currentSel.c_str());
        }
    }

    /* ── 3.  update local cache so the next change is correct ─ */
    if (currentRXTX[0] == 'R')
        g_bandStates[currentBand].rx = chosen;
    else
        g_bandStates[currentBand].tx = chosen;
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
