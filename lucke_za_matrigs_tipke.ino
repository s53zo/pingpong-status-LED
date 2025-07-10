// Include necessary libraries for ESP8266, networking, MQTT, EEPROM, LEDs, and JSON.
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <PubSubClient.h>
#include <EEPROM.h>
#include <WS2812FX.h>
#include <ArduinoJson.h>
#include <Ticker.h>
#include <vector>
#include <map>

// Include custom header for antenna MQTT handling logic.
#include "antenna_mqtt_handler.h"

// Define MQTT maximum packet size (increased for larger JSON payloads).
#define MQTT_MAX_PACKET_SIZE 2048

// Initialize WiFi, MQTT, and Web Server clients.
WiFiClient espClient;
PubSubClient client(espClient);
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

// --- Constants ---
#define VER "v1.39-debug jul 2025 S53ZO" // Firmware version string.
#define LED_PIN D3                       // GPIO pin for WS2812FX LEDs.
#define NUM_LEDS 4                       // Number of LEDs in the strip.
#define AP_SSID "ESP8266_Setup"          // SSID for Access Point fallback mode.

// Initialize WS2812FX LED control object.
WS2812FX ws2812fx = WS2812FX(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// --- Configuration Buffers (stored in EEPROM) ---
char ssid[32];         // WiFi SSID
char password[32];     // WiFi Password
char mqtt_server[40];  // MQTT Broker IP/Hostname
int mqtt_port = 1883;  // MQTT Broker Port
char station_name[32]; // Unique name for this station (e.g., RTX-XX)

// --- Runtime Buffers and State Variables ---
char logBuffer[256]; // General-purpose buffer for logging messages.

char macAddress[18]     = "";          // Device MAC address in string format.
char command_topic[64]  = "";          // MQTT topic for receiving commands (e.g., LED FX).
char feedback_topic[64] = "";          // MQTT topic for sending feedback/responses.
char debug_topic[64]    = "";          // MQTT topic for sending debug messages.
char currentRXTX[4]     = "RX";        // Current radio state: "RX" (receive) or "TX" (transmit).

char topic_band_prefix[64];   // MQTT topic prefix for band-specific data (e.g., "matrigs/0/sta/<sta>/b/").
char topic_band_wildcard[64]; // MQTT topic wildcard for subscribing to all band-specific data (e.g., "…/b/#").

bool mqttHelloRunning = false;    // Flag to indicate if the MQTT connection rainbow effect is active.
unsigned long mqttHelloStart = 0; // Timestamp when the MQTT hello effect started.

// --- Global State Flags and Structures ---
// Flag indicating if the radio is currently transmitting (PTT active).
bool g_txActive = false;

// Structure to hold a pending TX antenna change, used for safe switching.
PendingTxChange g_pendingTx = { "", "", "", false };

// --- Debugging Configuration ---
// Master switch for verbose debug output. Starts enabled for initial boot messages.
bool debugEnabled = true;
// Timestamp when debug messages should automatically disable (60 seconds after boot).
unsigned long debugDeadline = 0; // Initialized in setup() to be relative to boot time.

// --- Antenna and Band State ---
String currentBand = "?";             // Currently selected radio band (e.g., "40m").
String currentAntennas = "?";         // Space-separated list of antennas for the current band.
std::vector<String> currentAntList;   // Tokenized list of antennas for the current band.
std::map<String, String> bandCache;   // Cache: band name -> sorted antenna list string.

// --- MQTT Topic Buffers (derived from MAC address and station name) ---
char topic_cmd[128];       // Command topic (e.g., for LED FX commands).
char topic_available[128]; // Topic for antenna availability JSON.
char topic_dt[128];        // Legacy topic for combined status updates.

// -----------------------------------------------------------------
//  Debug Helper Function
//  Publishes debug messages to Serial and optionally to MQTT.
//  Only active if `debugEnabled` is true.
// -----------------------------------------------------------------
void publishDebugMessage(const char* msg) {
  // Exit early if debugging is not enabled.
  if (!debugEnabled) return;

  // Print message to Serial console.
  Serial.println(msg);

  // If MQTT client is connected, publish the debug message to the debug topic.
  if (client.connected()) {
    char debugTopic[100];
    // Construct the debug topic using the device's MAC address.
    snprintf(debugTopic, sizeof(debugTopic),
             "pingpong/%s/debug", WiFi.macAddress().c_str());
    client.publish(debugTopic, msg);
  }
}

// --- Web Configuration Pages (PROGMEM for memory efficiency) ---
// HTML head section for the configuration page.
const char PAGE_HEAD[] PROGMEM =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<title>ESP8266 Station Config</title><style>"
"body{font-family:Arial,Helvetica,sans-serif;margin:20px;color:#222;}"
"h1{color:#006edc;} input[type=text],input[type=number]{width:260px;}"
"input[type=submit]{padding:6px 18px;margin-top:10px;}"
"code{background:#f2f2f2;padding:2px 4px;border-radius:3px;}"
"</style></head><body>"
"<h1>Wi-Fi & MQTT Config</h1>";

// HTML form part 1: SSID input.
const char PAGE_FORM1[] PROGMEM =
"<form action='/save' method='post'>"
"SSID:<br><input name='ssid' type='text' value='";

// HTML form part 2: Password input.
const char PAGE_FORM2[] PROGMEM =
"'><br>Password:<br><input name='password' type='text' value='";

// HTML form part 3: MQTT Server IP input.
const char PAGE_FORM3[] PROGMEM =
"'><br>MQTT Server IP:<br><input name='mqtt_server' type='text' value='";

// HTML form part 4: MQTT Port input.
const char PAGE_FORM4[] PROGMEM =
"'><br>MQTT Port:<br><input name='mqtt_port' type='number' value='";

// HTML form part 5: Station Name input.
const char PAGE_FORM5[] PROGMEM =
"'><br>Station Name (RTX-XX):<br><input name='station_name' type='text' value='";

// HTML form part 6: Submit button.
const char PAGE_FORM6[] PROGMEM =
"'><br><input type='submit' value='Save & Reboot'></form>";

// HTML section for status display.
const char PAGE_STATUS_HEAD[] PROGMEM =
"<h2>Status</h2><ul>";

// HTML footer section, including OTA update link.
const char PAGE_FOOT[] PROGMEM =
"</ul><h2>OTA Update</h2>"
"<p>Open <a href='/update'>/update</a> to upload a new .bin.</p></body></html>";

// HTML cheat-sheet for LED commands (head part).
const char PAGE_CHEAT[] PROGMEM =
"<h2>LED Command Cheat-Sheet</h2>"
"<p>Publish to <code>";

// HTML cheat-sheet for LED commands (tail part).
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

// Helper function to send PROGMEM content to the web server.
static void send_P(const char* progmem)
{
  server.sendContent_P(progmem, strlen_P(progmem));
}

// -----------------------------------------------------------------
//  handleRoot()
//  Handles requests to the root URL (/). Displays configuration form
//  and current device status.
// -----------------------------------------------------------------
void handleRoot() {
  publishDebugMessage("Handling root page request");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");        // Send headers first.

  send_P(PAGE_HEAD);

  // Display configuration form with current values.
  send_P(PAGE_FORM1); server.sendContent(ssid);
  send_P(PAGE_FORM2); server.sendContent(password);
  send_P(PAGE_FORM3); server.sendContent(mqtt_server);
  send_P(PAGE_FORM4); server.sendContent(String(mqtt_port));
  send_P(PAGE_FORM5); server.sendContent(station_name);
  send_P(PAGE_FORM6);

  // Display device status information.
  send_P(PAGE_STATUS_HEAD);
  server.sendContent("<li>Firmware: <b>" + String(VER) + "</b></li>");
  server.sendContent("<li>MAC Address: <code>" + String(macAddress) + "</code></li>");
  server.sendContent("<li>Station: <b>" + String(station_name) + "</b></li>");
  server.sendContent("<li>Current band: " + currentBand + "</li>");
  server.sendContent("<li>Antennas for band: " + currentAntennas + "</li>");
  server.sendContent("<li>MQTT command topic: <code>" + String(command_topic) + "</code></li>");
  server.sendContent("<li>MQTT feedback topic: <code>" + String(feedback_topic) + "</code></li>");
  server.sendContent("<li>Debug topic: <code>" + String(debug_topic) + "</code></li>");
  send_P(PAGE_CHEAT);              // Static head for LED cheat sheet.
  server.sendContent(command_topic);   // Live command topic.
  send_P(PAGE_CHEAT_TAIL);         // List of LED commands and WS2812FX docs link.
  send_P(PAGE_FOOT);
}

// -----------------------------------------------------------------
//  handleUpdate()
//  Serves the OTA update page.
// -----------------------------------------------------------------
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

// -----------------------------------------------------------------
//  handleSave()
//  Handles form submission from the configuration page.
//  Saves new settings to EEPROM and reboots the ESP8266.
// -----------------------------------------------------------------
void handleSave() {
  // Copy submitted values to configuration buffers.
  strncpy(ssid, server.arg("ssid").c_str(), sizeof(ssid));
  strncpy(password, server.arg("password").c_str(), sizeof(password));
  strncpy(mqtt_server, server.arg("mqtt_server").c_str(), sizeof(mqtt_server));
  mqtt_port = server.arg("mqtt_port").toInt();
  strncpy(station_name, server.arg("station_name").c_str(), sizeof(station_name));

  // Initialize EEPROM and save configuration data.
  EEPROM.begin(512);
  EEPROM.put(0, ssid);
  EEPROM.put(32, password);
  EEPROM.put(64, mqtt_server);
  EEPROM.put(104, mqtt_port);
  EEPROM.put(108, station_name);
  EEPROM.commit(); // Commit changes to flash memory.
  EEPROM.end();    // Close EEPROM session.

  // Inform user and reboot.
  server.send(200, "text/html", "<html><body><h2>Saved. Rebooting...</h2></body></html>");
  delay(1000);
  ESP.restart();
}

// -----------------------------------------------------------------
//  setupWiFi()
//  Connects to the configured Wi-Fi network. If connection fails,
//  falls back to Access Point (AP) mode for configuration.
// -----------------------------------------------------------------
void setupWiFi()
{
    delay(10);
    Serial.println();
    Serial.printf("Connecting to \"%s\" …\n", ssid);

    // Set Wi-Fi to Station mode and begin connection.
    WiFi.mode(WIFI_STA);
#if defined(ESP32)
    WiFi.setHostname(station_name);
#else
    WiFi.hostname(station_name);
#endif
    WiFi.begin(ssid, password);

    // Wait up to 30 seconds for a Wi-Fi connection.
    const uint32_t CONNECT_TIMEOUT_MS = 30000;
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - t0 < CONNECT_TIMEOUT_MS) {
        delay(500);
        Serial.print('.');
    }

    // If connection failed, fall back to AP mode.
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\nTimeout – starting AP fallback");
        setupAP(); // Call helper to set up AP.
        return;    // Skip the rest of setupWiFi.
    }

    // Wi-Fi connected successfully.
    Serial.println();
    Serial.print("Connected! IP = ");
    Serial.println(WiFi.localIP());

    // Get MAC address and format it as ASCII "AA:BB:CC:DD:EE:FF".
#if defined(ESP32)
    uint8_t rawMac[6];
    WiFi.macAddress(rawMac);
    snprintf(macAddress, sizeof(macAddress),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             rawMac[0], rawMac[1], rawMac[2],
             rawMac[3], rawMac[4], rawMac[5]);
#else // ESP8266
    String macStr = WiFi.macAddress();
    macStr.toCharArray(macAddress, sizeof(macAddress));
#endif

    // Build MQTT topic strings using MAC address and station name.
    snprintf(command_topic,  sizeof(command_topic),
             "pingpong/%s/fxcmd",  macAddress);
    snprintf(feedback_topic, sizeof(feedback_topic),
             "pingpong/%s/fxresp", macAddress);
    snprintf(debug_topic,    sizeof(debug_topic),
             "pingpong/%s/debug",  macAddress);

    snprintf(topic_available, sizeof(topic_available),
             "matrigs/0/sta/%s/available", station_name);

    snprintf(topic_dt, sizeof(topic_dt),          // Legacy combined status feed.
             "matrigs/0/dt/RTX/d/%s", station_name);

    snprintf(topic_band_prefix, sizeof(topic_band_prefix),
             "matrigs/0/sta/%s/b/", station_name);
    snprintf(topic_band_wildcard, sizeof(topic_band_wildcard),
             "%s#", topic_band_prefix);

    // Subscribe to necessary MQTT topics.
    client.subscribe(command_topic);       // For LED FX commands.
    client.subscribe(topic_available);     // For antenna catalogue updates.
    client.subscribe(topic_dt);            // For legacy combined status.
    client.subscribe(topic_band_wildcard); // For live per-band JSON updates.

    // Print MQTT topic summary to console.
    Serial.print("Command topic: ");  Serial.println(command_topic);
    Serial.print("Feedback topic: "); Serial.println(feedback_topic);
    Serial.print("Debug topic: ");    Serial.println(debug_topic);
    Serial.print("Sub → ");           Serial.println(topic_available);
    Serial.print("Sub → ");           Serial.println(topic_dt);
    Serial.print("Sub → ");           Serial.println(topic_band_wildcard);

#if defined(ARDUINO_ARCH_ESP32)
    // Start mDNS responder for ESP32 (if applicable).
    if (MDNS.begin(station_name))
        Serial.println("[mDNS] responder started");
#endif
}

// -----------------------------------------------------------------
//  setupAP()
//  Configures the ESP8266 as an Access Point (AP).
// -----------------------------------------------------------------
void setupAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);
  Serial.printf("AP SSID: %s\n", AP_SSID);
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());
}

// -----------------------------------------------------------------
//  loadConfigFromEEPROM()
//  Loads saved configuration (WiFi, MQTT, station name) from EEPROM.
// -----------------------------------------------------------------
void loadConfigFromEEPROM() {
  EEPROM.begin(512);
  EEPROM.get(0, ssid);
  EEPROM.get(32, password);
  EEPROM.get(64, mqtt_server);
  EEPROM.get(104, mqtt_port);
  EEPROM.get(108, station_name);

  // Ensure strings are null-terminated after reading from EEPROM.
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

// -----------------------------------------------------------------
//  processFxCommand()
//  Parses and applies standard LED FX commands received via MQTT.
//  Expected format: "mode,brightness,speed,color_hex"
// -----------------------------------------------------------------
void processFxCommand(const char* msg) {
  int mode, brightness, speed;
  char colorStr[16];

  // Attempt to parse the message string.
  int parsed = sscanf(msg, "%d,%d,%d,%15s", &mode, &brightness, &speed, colorStr);
  if (parsed != 4) {
    publishDebugMessage("Standard command error: Error parsing message format. Expected 4 items.");
    return;
  }

  // Convert hex color string to unsigned long.
  unsigned long color = strtoul(colorStr, NULL, 16);

  // Apply LED settings and start the effect.
  ws2812fx.setMode(mode);
  ws2812fx.setBrightness(brightness);
  ws2812fx.setSpeed(speed);
  ws2812fx.setColor(color);
  ws2812fx.start();

  publishDebugMessage("✅ FX command applied.");
}

// -----------------------------------------------------------------
//  processSegmentCommand()
//  Parses and applies segment-specific LED FX commands.
//  Expected format: "segment,pixel_index,mode,brightness,speed,color_hex"
// -----------------------------------------------------------------
void processSegmentCommand(const char* msg)
{
  int segment, mode, brightness, speed;
  char colorStr[7] = {0}; // 6 chars + NUL terminator for hex color.
  uint32_t color   = 0;

  // Attempt to parse the message string.
  int parsed = sscanf(msg, "segment,%d,%d,%d,%d,%6s",
                      &segment, &mode, &brightness, &speed, colorStr);

  // Validate parsed values and segment index.
  if (parsed == 5 && segment >= 0 && segment < NUM_LEDS) {

      color = strtoul(colorStr, nullptr, 16);

      // Validate brightness, mode, and speed ranges.
      if (brightness < 0 || brightness > 255 ||
          mode < 0 || mode >= ws2812fx.getModeCount() ||
          speed < 0) {

          publishDebugMessage("[Segment] Invalid parameters");
          return;
      }

      // Apply segment LED settings.
      ws2812fx.setSegment(segment, segment, segment,
                          mode, color, speed, NO_OPTIONS);
      ws2812fx.setBrightness(brightness);
      ws2812fx.start();

      publishDebugMessage("[Segment] Command executed");
  }
  else {
      publishDebugMessage("[Segment] Parse error or invalid segment index.");
  }
}

// -----------------------------------------------------------------
//  callback()
//  MQTT message callback function. Dispatches messages to appropriate
//  handlers based on the topic.
// -----------------------------------------------------------------
void callback(char* topic, byte* payload, unsigned int length)
{
    // Static buffer to copy MQTT payload into (NUL-terminated).
    static const size_t MQTT_BUF_LEN = 2048;
    static char msg[MQTT_BUF_LEN];

    // Check for oversized payload.
    if (length >= MQTT_BUF_LEN) {
        Serial.printf("[MQTT] ⚠ oversized payload (%u bytes) – ignored\n",
                      length);
        return;
    }
    // Copy payload and ensure null-termination.
    memcpy(msg, payload, length);
    msg[length] = '\0';

    // Make a writable copy of the topic string for comparison.
    char topicCopy[160];
    strncpy(topicCopy, topic, sizeof(topicCopy) - 1);
    topicCopy[sizeof(topicCopy) - 1] = '\0';

    bool handled = false; // Flag to track if the message was handled.

    // 1. Handle "…/sta/<station>/available" topic (antenna catalogue).
    if (strcmp(topicCopy, topic_available) == 0) {
        handleAvailableJSON(msg);
        handled = true;
    }

    // 2. Handle legacy "…/dt/RTX/d/<station>" topic (combined status).
    else if (strcmp(topicCopy, topic_dt) == 0) {
        handleCurrentBandJSON(msg);
        handled = true;
    }

    // 3. Handle per-band live JSON "…/sta/<station>/b/<Band>" topic.
    else if (strncmp(topicCopy, topic_band_prefix,
                     strlen(topic_band_prefix)) == 0) {

        const char* bandTok = topicCopy + strlen(topic_band_prefix); // Extract band token.
        const char* slash   = strchr(bandTok, '/');

        // If no further slash, it's a base topic for band state.
        if (!slash) {
            handleBandStateJSON(bandTok, msg);
            handled = true;
        }
    }

    // 4. Handle LED FX commands "pingpong/<MAC>/fxcmd".
    else if (strcmp(topicCopy, command_topic) == 0) {

        if (strcmp(msg, "stop") == 0) { // Simple stop command.
            ws2812fx.stop();
            ws2812fx.clear();
            publishDebugMessage("[FX] 📴 stop");
        }
        else if (strncmp(msg, "segment,", 8) == 0) { // Advanced segment command.
            processSegmentCommand(msg);
        }
        else { // Standard FX command.
            processFxCommand(msg);
        }
        handled = true;
    }

    // 5. Log unknown or unhandled topics.
    if (!handled) {
        char dbg[192];
        snprintf(dbg, sizeof(dbg), "[MQTT] ⏭ ignored topic %s", topicCopy);
        publishDebugMessage(dbg);
    }
}

// -----------------------------------------------------------------
//  reconnectMQTT()
//  Attempts to reconnect to the MQTT broker if not already connected.
//  Displays a rainbow effect on LEDs upon successful connection.
// -----------------------------------------------------------------
void reconnectMQTT() {
  if (!client.connected()) {
    Serial.println("Attempting MQTT connection...");
    char clientId[64];
    // Generate a unique client ID using the MAC address.
    snprintf(clientId, sizeof(clientId), "PingPong-%s", WiFi.macAddress().c_str());

    if (client.connect(clientId)) {
      publishDebugMessage("MQTT connected successfully.");

      // Display a 5-second rainbow hello effect on LEDs.
      ws2812fx.setMode(11);                // FX_MODE_RAINBOW_CYCLE
      ws2812fx.setBrightness(108);
      ws2812fx.setSpeed(200);
      ws2812fx.setColor(0xFF0000);         // Color argument unused by mode 11.
      ws2812fx.start();

      mqttHelloRunning = true;
      mqttHelloStart   = millis();

      // Re-subscribe to MQTT topics after successful reconnection.
      char topic_cmd_local[128], topic1[128], topic2[128];
      strncpy(topic_cmd_local, command_topic, sizeof(topic_cmd_local));
      snprintf(topic1, sizeof(topic1), "matrigs/0/sta/%s/available", station_name);
      snprintf(topic2, sizeof(topic2), "matrigs/0/dt/RTX/d/%s", station_name);

      client.subscribe(topic_cmd_local);
      client.subscribe(topic1);
      client.subscribe(topic2);

      // Log subscription status.
      snprintf(logBuffer, sizeof(logBuffer), "Subscribed to command topic: %s", topic_cmd_local);
      publishDebugMessage(logBuffer);
      snprintf(logBuffer, sizeof(logBuffer), "Subscribed to: %s and %s", topic1, topic2);
      publishDebugMessage(logBuffer);

      // Publish a hello message to the feedback topic.
      snprintf(logBuffer, sizeof(logBuffer), "Hello %s", VER);
      char feedbackTopic[100];
      snprintf(feedbackTopic, sizeof(feedbackTopic), "pingpong/%s/fxresp", WiFi.macAddress().c_str());
      client.publish(feedbackTopic, logBuffer);
    }
  }
}

// -----------------------------------------------------------------
//  handleSerialCommands()
//  Reads commands from the USB Serial port and acts upon them.
//  Supports utility commands ("debug", "stop") and quick-select
//  antenna switching (digits 0-9).
// -----------------------------------------------------------------
void handleSerialCommands()
{
    // Return if no serial data is available.
    if (!Serial.available()) return;

    // Read one LF-terminated line and trim whitespace.
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (!cmd.length()) return; // Ignore empty lines.

    // --- Handle textual utility commands ---
    if (cmd.equalsIgnoreCase("debug")) {
        debugEnabled  = true;
        debugDeadline = millis() + 60000; // Auto-disable after 1 minute.
        Serial.println(F("🟢 Debug enabled for 1 minute"));
        publishDebugMessage("[Serial] debug ON (1 min)");
        return;
    }

    if (cmd.equalsIgnoreCase("stop")) { // Stop LED effects.
        ws2812fx.stop();
        ws2812fx.clear();
        Serial.println(F("[Serial] LEDs stopped"));
        publishDebugMessage("[Serial] LEDs stopped via USB");
        return;
    }

    // --- Handle quick-select: exactly one decimal digit 0-9 ---
    if (cmd.length() != 1 || !isDigit(cmd[0])) {
        Serial.println(F("[Serial] ❓ Unrecognised command – try a digit 0-9"));
        return;
    }

    int idx = cmd[0] - '0'; // Convert char digit to int.

    // Map digit to antenna name.
    String chosen;
    if (idx == 0) {
        chosen = F("LOAD-2KA"); // Special case: dummy load.
    } else if (idx <= (int)currentAntList.size()) {
        chosen = currentAntList[idx - 1]; // 1-based index to 0-based.
    } else {
        Serial.printf("[SerialSelect] Invalid antenna number %d\n", idx);
        return;
    }

    Serial.printf("[SerialSelect] %s band=%s → %s\n",
                  currentRXTX, currentBand.c_str(), chosen.c_str());

    // Find currently active antenna in this bank (RX or TX).
    BandState curState  = g_bandStates[currentBand];
    String    currentSel = (currentRXTX[0] == 'R') ? curState.rx
                                                   : curState.tx;

    // 1. If TX bank and PTT is active, queue the change.
    if (currentRXTX[0] == 'T' && g_txActive) {
        g_pendingTx.band   = currentBand;
        g_pendingTx.oldAnt = currentSel;
        g_pendingTx.newAnt = chosen;
        g_pendingTx.valid  = true;
        publishDebugMessage("[TX-Queue] 💤 queued until RTX returns to RX");
        return;
    }

    // 2. Publish immediately (if RX, or TX while in RX).
    if (!client.connected()) {
        publishDebugMessage("[SerialSelect] ⚠ MQTT not connected");
    } else if (currentSel == chosen) {
        publishDebugMessage("[SerialSelect] 🔄 already active");
    } else {
        // Remove old antenna (if any) by publishing to MQTT.
        if (currentSel.length()) {
            char topicR[128];
            snprintf(topicR, sizeof(topicR),
                     "matrigs/0/sta/%s/b/%s/p:remove/%sANTENNAS",
                     station_name, currentBand.c_str(), currentRXTX);
            client.publish(topicR, currentSel.c_str());
        }
        // Add new antenna by publishing to MQTT.
        char topicA[128];
        snprintf(topicA, sizeof(topicA),
                 "matrigs/0/sta/%s/b/%s/p:add/%sANTENNAS",
                 station_name, currentBand.c_str(), currentRXTX);
        client.publish(topicA, chosen.c_str());
    }

    // 3. Update local cache with the chosen antenna.
    if (currentRXTX[0] == 'R')
        g_bandStates[currentBand].rx = chosen;
    else
        g_bandStates[currentBand].tx = chosen;
}

// -----------------------------------------------------------------
//  setup()
//  Arduino setup function. Initializes serial, loads config,
//  sets up WiFi, web server, and LED effects.
// -----------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("Booting...");
  Serial.println(VER);
  // Set absolute timestamp for debug auto-disable (60 seconds from now).
  debugDeadline = millis() + 60000;
  Serial.println(F("🟢 Debug enabled for 1 minute after boot"));

  loadConfigFromEEPROM(); // Load saved WiFi and MQTT settings.
  setupWiFi();            // Connect to WiFi or start AP.

  // Configure web server routes.
  server.on("/", handleRoot);
  server.on("/save", handleSave);
  server.on("/update", HTTP_GET, handleUpdate);
  httpUpdater.setup(&server);
  server.begin();
  Serial.println("HTTP server started");

  // Initialize WS2812FX LED strip.
  ws2812fx.init();
  ws2812fx.setBrightness(100);
  ws2812fx.setSpeed(500);
  ws2812fx.setColor(0x0000FF);
  ws2812fx.setMode(FX_MODE_STATIC);
  ws2812fx.start();

  // Configure MQTT client.
  client.setBufferSize(2048);
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

// -----------------------------------------------------------------
//  loop()
//  Arduino loop function. Continuously handles serial commands,
//  LED effects, web server clients, and MQTT communication.
// -----------------------------------------------------------------
void loop() {
  handleSerialCommands(); // Process commands from USB Serial.

  // Auto-disable debug messages after the initial minute.
  if (debugEnabled && debugDeadline && millis() >= debugDeadline) {
      debugEnabled   = false;
      debugDeadline  = 0; // Stop checking.
      Serial.println(F("🔴 Debug messages DISABLED (1-minute timeout)"));
  }

  static unsigned long lastMQTTCheck = 0;
  unsigned long now = millis();

  ws2812fx.service();      // Update LED effects.
  server.handleClient();   // Handle incoming web requests.
  client.loop();           // Process MQTT messages and maintain connection.

  // Turn off MQTT hello rainbow effect after 5 seconds.
  if (mqttHelloRunning && millis() - mqttHelloStart >= 5000) {
      ws2812fx.stop();
      ws2812fx.clear(); // Blank the LED strip.
      mqttHelloRunning = false;
  }

  // Handle WiFi connection status changes.
  if (WiFi.status() != WL_CONNECTED) {
    if (WiFi.getMode() != WIFI_AP) {
      publishDebugMessage("Loop: WiFi disconnected. Switching to AP mode indication.");
      ws2812fx.setMode(FX_MODE_BLINK);
      ws2812fx.setColor(0xFF0000);
      ws2812fx.start();
    }
    if (client.connected()) {
      publishDebugMessage("Loop: WiFi lost, disconnecting MQTT.");
      client.disconnect();
    }
  } else { // WiFi is connected.
    if (WiFi.getMode() != WIFI_STA) {
      publishDebugMessage("Loop: WiFi connected! Switching from AP to STA indication.");
      WiFi.mode(WIFI_STA);
      ws2812fx.stop();
      ws2812fx.clear();
    }
  }

  // Attempt to reconnect to MQTT if disconnected (every 5 seconds).
  if (!client.connected() && (now - lastMQTTCheck > 5000)) {
    lastMQTTCheck = now;
    reconnectMQTT();
  }
}
