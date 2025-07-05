Of course\! Here is a `readme.md` file for your GitHub project, generated based on the code you provided.

-----

# ESP8266 MQTT Antenna & LED Controller

This project provides a flexible and configurable solution for controlling an antenna system and a WS2812B LED strip via MQTT. It is designed to be used in a radio amateur context (inspired by S53ZO's setup), where it dynamically receives antenna availability for different bands and allows for selection.

The device is built on an ESP8266, configurable via a web interface, and supports Over-the-Air (OTA) updates.

## ✨ Features

  * **Web-Based Configuration**: No need to hardcode credentials. The ESP8266 hosts a web portal to configure WiFi, MQTT broker details, and a station name.
  * **AP Fallback Mode**: If the configured WiFi is unavailable, the device starts an Access Point (`ESP8266_Setup`) to allow for initial setup or reconfiguration.
  * **Dynamic Antenna Management**: Subscribes to an MQTT topic that provides a JSON list of available antennas and their supported bands.
  * **Natural Alphanumeric Sorting**: Correctly sorts antenna names like "A1", "A2", "A10", "A-H".
  * **Band-Aware Antenna Lists**: Responds to MQTT messages about the current operating band and prepares a corresponding list of available antennas.
  * **MQTT & Serial Control**:
      * Control WS2812FX LED effects via MQTT commands.
      * Select antennas using simple numeric commands over the USB serial port.
  * **Status LEDs**: Uses a WS2812B LED strip to provide visual feedback on WiFi/MQTT connection status and a "hello" animation on boot.
  * **OTA Updates**: A built-in web page allows for easy firmware updates over WiFi.
  * **Persistent Configuration**: Saves all settings to EEPROM.
  * **Remote Debugging**: Publishes detailed debug messages to a dedicated MQTT topic.

## ⚙️ Hardware Requirements

  * An ESP8266-based board (e.g., NodeMCU, Wemos D1 Mini).
  * A WS2812B addressable LED strip (configured for 4 LEDs by default).
  * A suitable 5V power supply for the ESP8266 and the LED strip.

## 📚 Libraries Used

This project relies on the following Arduino libraries:

  * `ESP8266WiFi`
  * `ESP8266WebServer`
  * `ESP8266HTTPUpdateServer`
  * `PubSubClient` (v2.8.0 or compatible, with `MQTT_MAX_PACKET_SIZE` increased)
  * `EEPROM`
  * `WS2812FX`
  * `ArduinoJson` (v6 or compatible)

## 🚀 Setup and Configuration

1.  **Flash Firmware**: Compile and upload the sketch to your ESP8266 board using the Arduino IDE or PlatformIO.
2.  **Connect to AP**: On its first boot or if it fails to connect to a saved network, the device will create a WiFi Access Point with the SSID: **`ESP8266_Setup`**. Connect your computer or phone to this network.
3.  **Open Web Portal**: Once connected, open a web browser and navigate to `http://192.168.4.1`.
4.  **Enter Details**: You will see a configuration page. Fill in your:
      * WiFi SSID & Password
      * MQTT Server IP & Port
      * Station Name (e.g., `RTX-01`)
5.  **Save and Reboot**: Click "Save & Reboot". The device will restart and attempt to connect to your WiFi network and MQTT broker.

## 📡 MQTT Protocol

The device uses a structured set of MQTT topics for operation. The `<mac_address>` is the device's MAC address (e.g., `AA:BB:CC:DD:EE:FF`), and the `<station_name>` is the name you configured in the web portal.

### Subscribed Topics

The device listens for messages on the following topics:

  * **`matrigs/0/sta/<station_name>/available`**
      * **Purpose**: To receive the master list of all available antennas. The payload should be a JSON object where keys are antenna names.
      * **Example Payload**:
        ```json
        {
          "A1-V": { "80m": ["RX"], "40m": ["RX","TX"] },
          "A2-H": { "40m": ["RX"], "20m": ["TX"] },
          "LOAD-2KA": { "80m": ["TX"], "40m": ["TX"] }
        }
        ```
  * **`matrigs/0/dt/RTX/d/<station_name>`**
      * **Purpose**: To inform the device of the current operating band.
      * **Example Payload**:
        ```json
        { "BANDS": "40m", "TARGET": { "RXTX": "RX" } }
        ```
  * **`pingpong/<mac_address>/fxcmd`**
      * **Purpose**: To control the WS2812FX LED strip.
      * **Payloads**:
          * `stop`: Turns the LEDs off.
          * `<mode>,<brightness>,<speed>,<color_hex>`: Sets a standard effect. Example: `1,128,200,00FF00` for a medium-speed green blink.
          * `segment,<pixel>,<mode>,<bright>,<speed>,<color_hex>`: Controls a single pixel. Example: `segment,0,0,255,0,FF0000` to set the first pixel to static red.

### Published Topics

The device publishes messages to the following topics:

  * **`matrigs/0/sta/<station_name>/b/<band>/p:set/[RX|TX]ANTENNAS`**
      * **Purpose**: Publishes the chosen antenna after a selection is made via the serial interface.
      * **Example Payload**: `["A1-V"]`
  * **`pingpong/<mac_address>/debug`**
      * **Purpose**: Publishes verbose logs about its state and received messages. This can be enabled/disabled via the serial interface.
  * **`pingpong/<mac_address>/fxresp`**
      * **Purpose**: Publishes a "hello" message on successful MQTT connection.

## 💻 Serial Interface

Connect to the ESP8266 using a serial monitor at **115200 baud**. The following commands are available:

  * `debug`: Enables publishing verbose logs to the `.../debug` MQTT topic. (Enabled by default for the first 60 seconds after boot).
  * `stop`: Disables publishing debug logs.
  * `<number>` (e.g., `1`, `2`): Selects an antenna from the currently active list.
      * `0`: A special command to always select the `LOAD-2KA` antenna.
      * `1`, `2`, `3`...: Selects the corresponding antenna from the sorted list for the current band (e.g., `1` selects the first antenna in the list).

The current antenna list is printed to the serial monitor when the band changes.

## 📂 Project Files

  * **`[sketch_name].ino`**: The main sketch containing `setup()` and `loop()`. It handles WiFi, the web server, OTA updates, and MQTT connection logic.
  * **`antenna_mqtt_handler.h`**: Header file for the antenna data processing module.
  * **`antenna_mqtt_handler.cpp`**: Implementation for parsing, storing, and sorting antenna information received from MQTT. It contains the core logic for managing antenna lists based on the current band.
