Circuit board:
https://oshwlab.com/s53zo/pingpong-lucke

Short description:
This code configures an ESP8266 microcontroller to connect to WiFi, host a web interface for setup, control WS2812 LEDs, and communicate with an MQTT server for remote commands. It allows users to easily configure network settings, update firmware over the air, and manage LED lighting effects remotely through MQTT, making it ideal for smart home applications and IoT projects. The device automatically handles reconnections and offers power-saving features by turning off LEDs when not in use.

Longer description:
This code transforms an ESP8266 microcontroller, I use NodeMCU 0.9, into a smart WiFi and LED controller with MQTT integration. Here's what it does:

1. **WiFi Configuration and Setup:**
   - When you first power on the device, it either connects to your existing WiFi network or sets up its own WiFi network (called "ESP8266_Setup") if it can't connect.
   - You can connect to this network from your phone or computer and access a setup page at the default IP address.

2. **Web Interface for Configuration:**
   - The device hosts a simple web page where you can enter your WiFi network name (SSID), password, and MQTT server details (IP address and port).
   - Once you submit this information, the device saves it and restarts to connect to your specified network and MQTT server.

3. **LED Control through MQTT:**
   - The ESP8266 controls a strip of WS2812 LEDs, allowing you to change the colors, brightness, and patterns through MQTT messages.
   - You can send specific commands like turning the LEDs on and off, changing colors, or setting them to blink in different patterns via an MQTT topic.

4. **Over-the-Air (OTA) Firmware Updates:**
   - The device supports OTA updates, meaning you can upload new firmware directly through the web interface without needing a physical connection.

5. **Automatic LED Turn-Off:**
   - If you don't send any LED commands for 60 minutes, the device will automatically turn off the LEDs to save power.

6. **Feedback and Status:**
   - The device provides feedback over MQTT and through the web interface, so you always know if your commands are received and executed.
   - It also periodically sends "Alive" messages to let you know it’s still connected and functioning.

7. **Built-in Reconnect and Recovery:**
   - If the device loses its connection to the WiFi or MQTT server, it automatically tries to reconnect. If it can't connect to WiFi, it goes back into setup mode, making it easy to reconfigure.

Overall, this code makes the ESP8266 a flexible and user-friendly tool for smart home projects, particularly those involving LED lighting and remote control through WiFi and MQTT.

MQTT messages can be sent to control the LEDs with the following format:

Static mode: 0,255,100,FF0000 - Mode 0, full brightness, speed 100, red color.
Blink mode: 1,128,200,00FF00 - Mode 1, half brightness, speed 200, green color.
Turn off LEDs: stop
Segment control: segment,0,2,255,100,FF0000 - LED 0, mode 2, full brightness, speed 100, red color.

See https://github.com/kitesurfer1404/WS2812FX\ GitHub page for more details.
