# Repository Guidelines

## Project Structure & Module Organization

- `pingpong.FX_LEDs.ino`: primary ESP8266 firmware (WiFi config UI, MQTT, WS2812FX, MatriGS integration).
- `pingpong-status-LED.ino`: empty sketch entrypoint so Arduino tooling accepts this folder as a sketch.
- `antenna_mqtt_handler.cpp` / `antenna_mqtt_handler.h`: MatriGS JSON helpers, antenna sorting/caches, TX-queue logic.
- `legacy/pingpong.FX_LEDs.ino`: older LED-only firmware kept for reference.
- `pingpong*.jpg`, `Pingpong skatla.stl`: documentation/assets.

There are no automated tests in this repo; validation is compile + on-device smoke testing.

## Build, Test, and Development Commands

- Build (NodeMCU):
  - `arduino-cli compile --fqbn esp8266:esp8266:nodemcu .`
- Upload (example, set your port):
  - `arduino-cli upload --fqbn esp8266:esp8266:nodemcu --port /dev/tty.usbserial-XXXX .`
- Helpful:
  - `arduino-cli lib list` (confirm `WS2812FX`, `PubSubClient`, `ArduinoJson` installed)

## Coding Style & Naming Conventions

- Follow existing Arduino/C++ style in the file you are editing; prefer 2-space indentation and no tabs.
- Names:
  - Functions: `lowerCamelCase` (e.g., `handleSerialCommands`).
  - Shared globals/state: `g_*` (e.g., `g_txActive`, `g_pendingTx`).
  - MQTT topics/buffers: `topic_*` / `*_topic` and fixed-size `char[]` buffers.
- Keep memory in mind (ESP8266): avoid large temporary `String` concatenations inside tight loops.

## Testing Guidelines

Minimum checklist for changes:
- `arduino-cli compile ...` succeeds for `esp8266:esp8266:nodemcu`.
- Device smoke test:
  - `GET /` loads and shows MAC/station info.
  - `GET /update` renders OTA form.
  - MQTT: verify `pingpong/<MAC>/fxcmd` commands and expected subscriptions/publishes.

## Commit & Pull Request Guidelines

- Commit messages in history are short and imperative (e.g., `Update README.md`). Keep that style; add a scope when helpful (e.g., `firmware: fix MQTT resubscribe`).
- PRs should include:
  - Target board + wiring assumptions (LED pin `D3`, `NUM_LEDS`).
  - Any MQTT topic/payload changes.
  - Note if EEPROM layout or `/update` behavior changes.
  - Compile confirmation output (or the exact command used).

## Security & Configuration Tips

- WiFi/MQTT settings are entered via the web UI and stored in EEPROM; avoid committing real credentials.
- OTA (`/update`) is intentionally unauthenticated in this firmware: don’t expose the device on untrusted networks.

