#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <WiFiClient.h>
#include <EEPROM.h>
#include <WS2812FX.h>
#include <PubSubClient.h>

#define VER "v1.38 jul 2024 S53ZO"

#define LED_PIN D3
#define NUM_LEDS 4
#define AP_SSID "ESP8266_Setup"

WS2812FX ws2812fx = WS2812FX(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

char ssid[32];
char password[32];
char mqtt_server[40];
int mqtt_port;

WiFiClient espClient;
PubSubClient client(espClient);

String macAddress;
unsigned long previousMillis = 0;
unsigned long lastLEDCommandMillis = 0;
const long interval = 300000; // 5 minutes interval
const long LED_OFF_INTERVAL = 3600000; // 60 minutes in milliseconds

void handleRoot() {
  Serial.println("Handling root page");
  String macAddress = WiFi.macAddress();
  String html = "<html><body><h1>WiFi Config</h1>"
                "<form action=\"/save\" method=\"post\">"
                "SSID: <input type=\"text\" name=\"ssid\" value=\"" + String(ssid) + "\"><br>"
                "Password: <input type=\"text\" name=\"password\" value=\"" + String(password) + "\"><br>"
                "MQTT Server IP: <input type=\"text\" name=\"mqtt_server\" value=\"10.0.10.9\"><br>"
                "MQTT Server Port: <input type=\"number\" name=\"mqtt_port\" value=\"1883\"><br>"
                "<input type=\"submit\" value=\"Save\">"
                "</form>"
                "<h2>Instructions</h2>"
                "<p>Use this form to configure the WiFi and MQTT settings for your device.</p>"
                "<p>Confirmation is received on <code>pingpong/" + macAddress + "/fxresp</code> over MQTT</p>"
                "<p>MQTT messages can be sent to control the LEDs with the following format:</p>"
                "<ul>"
                "<li>Static mode: <code>0,255,100,FF0000</code> - Mode 0, full brightness, speed 100, red color.</li>"
                "<li>Blink mode: <code>1,128,200,00FF00</code> - Mode 1, half brightness, speed 200, green color.</li>"
                "<li>Turn off LEDs: <code>stop</code></li>"
                "<li>Segment control: <code>segment,0,2,255,100,FF0000</code> - LED 0, mode 2, full brightness, speed 100, red color.</li>"
                "</ul>"
                "<p>See <a href=\"https://github.com/kitesurfer1404/WS2812FX\">WS2812FX GitHub page</a> for more details.</p>"
                "<p>To update the firmware, go to <a href=\"/update\">/update</a></p>"
                "</body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  String ssid_str = server.arg("ssid");
  String password_str = server.arg("password");
  String mqtt_server_str = server.arg("mqtt_server");
  String mqtt_port_str = server.arg("mqtt_port");

  ssid_str.toCharArray(ssid, sizeof(ssid));
  password_str.toCharArray(password, sizeof(password));
  mqtt_server_str.toCharArray(mqtt_server, sizeof(mqtt_server));
  mqtt_port = mqtt_port_str.toInt();

  EEPROM.begin(512);
  EEPROM.put(0, ssid);
  EEPROM.put(32, password);
  EEPROM.put(64, mqtt_server);
  EEPROM.put(104, mqtt_port);
  EEPROM.commit();

  String html = "<html><body><h1>Saved. Rebooting...</h1></body></html>";
  server.send(200, "text/html", html);
  delay(2000);
  ESP.restart();
}

void handleUpdate() {
  String html = "<html><body><h1>OTA Update</h1>"
                "<form method='POST' action='/update' enctype='multipart/form-data'>"
                "<input type='file' name='update'>"
                "<input type='submit' value='Update'>"
                "</form>"
                "</body></html>";
  server.send(200, "text/html", html);
}

void setup() {
  Serial.begin(115200);
  EEPROM.begin(512);
  EEPROM.get(0, ssid);
  EEPROM.get(32, password);
  EEPROM.get(64, mqtt_server);
  EEPROM.get(104, mqtt_port);

  ws2812fx.init();
  ws2812fx.setBrightness(100);
  pinMode(LED_BUILTIN, OUTPUT);

  Serial.println("Stored SSID: " + String(ssid));
  Serial.println("Stored Password: " + String(password));
  Serial.println("Stored MQTT Server: " + String(mqtt_server));
  Serial.println("Stored MQTT Port: " + String(mqtt_port));

  macAddress = WiFi.macAddress();
  Serial.print("MAC Address: ");
  Serial.println(macAddress);

  if (strlen(ssid) > 0 && strlen(password) > 0) {
    setup_wifi();
  } else {
    setupAP();
  }

  server.on("/", handleRoot);
  server.on("/save", handleSave);
  server.on("/update", HTTP_GET, handleUpdate);
  server.begin();
  Serial.println("HTTP server started");

  // Setup OTA
  httpUpdater.setup(&server);
  Serial.println("OTA HTTP server ready");
}

void setup_wifi() {
  WiFi.begin(ssid, password);
  Serial.print("Connecting to ");
  Serial.println(ssid);

  int wifi_timeout = 0;
  while (WiFi.status() != WL_CONNECTED && wifi_timeout < 30) {
    delay(500);
    Serial.print(".");
    wifi_timeout++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nFailed to connect to WiFi. Entering AP mode.");
    setupAP();
  } else {
    Serial.println("\nWiFi connected");
    printWiFiStatus();
    connect_mqtt();
  }
}

void setupAP() {
  Serial.println("Setting up AP mode...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);

  ws2812fx.setMode(FX_MODE_STATIC); // Fallback mode for AP mode
  ws2812fx.setBrightness(100);
  ws2812fx.setColor(RED); // Red color
  ws2812fx.start();
}

void printUptime() {
  unsigned long millisec = millis();
  unsigned long seconds = millisec / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;
  unsigned long days = hours / 24;

  millisec %= 1000;
  seconds %= 60;
  minutes %= 60;
  hours %= 24;

  Serial.print("Uptime: ");
  if (days > 0) {
    Serial.print(days);
    Serial.print("d ");
  }
  if (hours > 0) {
    Serial.print(hours);
    Serial.print("h ");
  }
  if (minutes > 0) {
    Serial.print(minutes);
    Serial.print("m ");
  }
  Serial.print(seconds);
  Serial.print("s ");
  Serial.print(millisec);
  Serial.println("ms");
}

void connect_mqtt() {
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
  client.setKeepAlive(60); // Set keep-alive interval to 60 seconds

  String clientId = "PingPong-" + macAddress;

  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    printUptime(); // Print uptime information
    if (client.connect(clientId.c_str())) {
      Serial.println("connected");
      ws2812fx.setMode(FX_MODE_TWINKLE);
      ws2812fx.setBrightness(100);
      ws2812fx.setColor(WHITE);
      ws2812fx.setSpeed(200);
      ws2812fx.start();
      delay(3000);
      ws2812fx.resetSegments();
      ws2812fx.stop();

      String command_topic = "pingpong/" + macAddress + "/fxcmd";
      String feedback_topic = "pingpong/" + macAddress + "/fxresp";
      client.subscribe(command_topic.c_str());

      // Say hello to feedback topic
      client.publish(feedback_topic.c_str(), ("Hello " + String(VER)).c_str());
      Serial.println("");
      Serial.println("Hello " + String(VER));
    } else {
      Serial.print("failed, rc=");
      int state = client.state();
      Serial.print(state);
      Serial.print(" - ");
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.println(message);

  // Reset the timer whenever an LED command is received
  lastLEDCommandMillis = millis();

  String feedback = "OK";
  if (message == "stop") {
    ws2812fx.resetSegments();
    ws2812fx.stop();
  } else if (message.startsWith("segment")) {
    int segment, mode, brightness, speed;
    char colorStr[7];
    uint32_t color;
    int parsed = sscanf(message.c_str(), "segment,%d,%d,%d,%d,%6s", &segment, &mode, &brightness, &speed, colorStr);
    if (parsed == 5 && segment < NUM_LEDS) {
      colorStr[6] = '\0';
      color = (uint32_t) strtol(colorStr, NULL, 16);

      ws2812fx.setSegment(segment, segment, segment, mode, color, speed, false);
      ws2812fx.setBrightness(brightness);
      ws2812fx.start();
    } else {
      feedback = "Error parsing segment message";
      Serial.println("Error parsing segment message");
    }
  } else {
    int mode, brightness, speed;
    char colorStr[7];
    uint32_t color = BLACK; // Default to BLACK if parsing fails
    int parsed = sscanf(message.c_str(), "%d,%d,%d,%6s", &mode, &brightness, &speed, colorStr);
    if (parsed == 4) {
      colorStr[6] = '\0';
      color = (uint32_t) strtol(colorStr, NULL, 16);

      ws2812fx.resetSegments();
      ws2812fx.setSegment(0, 0, NUM_LEDS - 1, mode, color, speed, false);
      ws2812fx.setBrightness(brightness);
      ws2812fx.start();
    } else {
      feedback = "Error parsing message";
      Serial.println("Error parsing message");
    }
  }

  // Ensure MQTT client is connected before publishing feedback
  if (client.connected()) {
    String feedback_topic = "pingpong/" + macAddress + "/fxresp";
    Serial.print("Publishing feedback to topic: ");
    Serial.println(feedback_topic);
    client.publish(feedback_topic.c_str(), feedback.c_str());
  } else {
    Serial.println("MQTT client not connected. Cannot publish feedback.");
  }
}

//void blinkLED() {
//  digitalWrite(LED_BUILTIN, LOW);
//  delay(200);
//  digitalWrite(LED_BUILTIN, HIGH);
//  delay(200);
//}

void printWiFiStatus() {
  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());

  IPAddress ip = WiFi.localIP();
  Serial.print("IP Address: ");
  Serial.println(ip);

  long rssi = WiFi.RSSI();
  Serial.print("Signal strength (RSSI): ");
  Serial.print(rssi);
  Serial.println(" dBm");

  const uint8_t* bssid = WiFi.BSSID();
  Serial.print("BSSID: ");
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X\n", bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
}


void loop() {
  server.handleClient();
  if (WiFi.status() == WL_CONNECTED) {
    // Handle MQTT client loop only if connected to WiFi
    if (!client.connected()) {
      Serial.println("MQTT client not connected, attempting to reconnect...");
      connect_mqtt();
    } else {
      client.loop(); // Ensure this is called frequently
      // Additional debug to print MQTT connection status
      static unsigned long lastMQTTStatusCheck = 0;
      unsigned long currentMillis = millis();
      if (currentMillis - lastMQTTStatusCheck >= 10000) { // Check every 10 seconds
        lastMQTTStatusCheck = currentMillis;
        int state = client.state();
      }
    }

    // Every 5 minutes, publish "Alive" to feedback topic
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= interval) {
      previousMillis = currentMillis;
      String feedback_topic = "pingpong/" + macAddress + "/fxresp";
      client.publish(feedback_topic.c_str(), "Alive");
      Serial.println("Published 'Alive' to MQTT topic");
    }

    // Check if 60 minutes have passed since the last LED command
    if (currentMillis - lastLEDCommandMillis >= LED_OFF_INTERVAL) {
      Serial.println("60 minutes passed since last LED command. Turning off LEDs.");
      ws2812fx.resetSegments();
      ws2812fx.stop();
      lastLEDCommandMillis = currentMillis; // Reset the timer to avoid repeated messages
    }

  } else {
    Serial.println("WiFi disconnected, attempting to reconnect...");
    WiFi.reconnect();
    delay(5000); // Wait before trying again
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("WiFi reconnected");
      printWiFiStatus();
    } else if (WiFi.getMode() != WIFI_AP) {
      // If not connected to WiFi and not already in AP mode, set up AP
      setupAP();
    }
  }
  ws2812fx.service();
}
