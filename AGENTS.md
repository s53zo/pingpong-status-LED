# Repository Guidelines

**IMPORTANT:** Power the MCP23008 keypad board from the **3.3V** pin. Do **not** use 5V.

## Project Structure & Module Organization

- `pingpong.FX_LEDs.ino`: primary ESP8266 firmware (WiFi config UI, MQTT, WS2812FX, MatriGS integration, input handling).
- `pingpong-status-LED.ino`: empty entrypoint so Arduino tooling accepts this sketch.
- `antenna_mqtt_handler.cpp` / `antenna_mqtt_handler.h`: MatriGS JSON helpers, antenna sorting/caches, TX-queue logic.
- `dongutec_keypad_mcp23008.cpp` / `dongutec_keypad_mcp23008.h`: optional external 4x4 keypad over I2C via MCP23008.
- `legacy/pingpong.FX_LEDs.ino`: older LED-only firmware kept for reference.
- `pingpong*.jpg`, `Pingpong skatla.stl`: documentation/assets.

No automated tests; validate by compile + on-device smoke testing.

## Build, Test, and Development Commands

- Build (NodeMCU):
  - `arduino-cli compile --fqbn esp8266:esp8266:nodemcu .`
- Upload (example, set your port):
  - `arduino-cli upload --fqbn esp8266:esp8266:nodemcu --port /dev/tty.usbserial-XXXX .`
- Helpful:
  - `arduino-cli lib list` (confirm `WS2812FX`, `PubSubClient`, `ArduinoJson` installed)

## Coding Style & Naming Conventions

- Follow existing style; prefer 2-space indentation (no tabs).
- Names:
  - Functions: `lowerCamelCase` (e.g., `handleSerialCommands`).
  - Shared globals/state: `g_*` (e.g., `g_txActive`, `g_pendingTx`).
  - MQTT topics/buffers: `topic_*` / `*_topic` and fixed-size `char[]` buffers.
- Keep memory in mind (ESP8266): avoid large temporary `String` concatenations inside tight loops.
- Keypad orientation differs between builds; adjust the `labels[16]` mapping in `pingpong.FX_LEDs.ino` if key labels don’t match your physical keypad.

## Testing Guidelines

Minimum checklist for changes:
- `arduino-cli compile ...` succeeds for `esp8266:esp8266:nodemcu`.
- Device smoke test:
  - `GET /` loads and shows MAC/station info.
  - `GET /update` renders OTA form.
  - MQTT: verify `pingpong/<MAC>/fxcmd` commands and expected subscriptions/publishes.
  - Keypad (if connected): status shows `Keypad (MCP23008): present`, and pressing keys triggers the same actions as serial number input.

## Commit & Pull Request Guidelines

- Commit messages in history are short and imperative (e.g., `Update README.md`). Keep that style; add a scope when helpful (e.g., `firmware: fix MQTT resubscribe`).
- PRs should include:
  - Target board + wiring assumptions (LED pin `D3`, `NUM_LEDS`).
  - If keypad-related: I2C pins (`SDA=D2`, `SCL=D1`), 3.3V power, and I2C address (default `0x27`).
  - Any MQTT topic/payload changes.
  - Note if EEPROM layout or `/update` behavior changes.
  - Compile confirmation output (or the exact command used).

## Security & Configuration Tips

- WiFi/MQTT settings are entered via the web UI and stored in EEPROM; avoid committing real credentials.
- OTA (`/update`) is intentionally unauthenticated in this firmware: don’t expose the device on untrusted networks.
