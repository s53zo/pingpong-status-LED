# Repository Guidelines

**IMPORTANT:** Power the MCP23008 keypad board from the **3.3V** pin. Do **not** use 5V.

## Project Structure & Module Organization

- `pingpong.FX_LEDs.ino`: main ESP8266 firmware (web config UI, WS2812FX, keypad, MatriGS integration).
- `pingpong-status-LED.ino`: thin sketch entrypoint for Arduino tooling.
- `antenna_mqtt_handler.*`: MatriGS JSON parsing, antenna sorting/caching, TX-queue logic.
- `dongutec_keypad_mcp23008.*`: Dongutec 4x4 keypad driver (MCP23008 over I2C).
- `legacy/`: older firmware snapshots kept for reference.

## Build, Test, and Development Commands

- Build: `arduino-cli compile --fqbn esp8266:esp8266:nodemcu .`
- Build to folder: `arduino-cli compile --fqbn esp8266:esp8266:nodemcu --output-dir build/out .`
- OTA upload: `curl -F "update=@build/out/pingpong-status-LED.ino.bin" http://<device-ip>/update`
- Serial upload: `arduino-cli upload --fqbn esp8266:esp8266:nodemcu --port /dev/tty.usbserial-XXXX .`
- Debug tail (Pingpong broker): `mosquitto_sub -h <broker> -p 1883 -t 'pingpong/<MAC>/debug' -v`
- MatriGS tail (MatriGS broker): `mosquitto_sub -h <broker> -p 4883 -t 'matrigs/0/sta/<STA>/#' -v`

## Architecture Overview

- Two MQTT connections are used:
  - `clientCmd` (Pingpong broker): LED commands `pingpong/<MAC>/fxcmd` and debug `pingpong/<MAC>/debug`.
  - `clientMatrigs` (MatriGS broker): `matrigs/0/sta/<STA>/available`, legacy DT feed, per-band state, and antenna switch publishes (`p:add/*ANTENNAS`, `p:remove/*ANTENNAS`).

## Coding Style & Naming Conventions

- Prefer 2-space indentation and fixed-size `char[]` topic buffers (ESP8266 RAM is tight).
- Globals use `g_*`; topics use `topic_*` / `*_topic`.
- When publishing/subscribing, pick the correct broker (`clientCmd` vs `clientMatrigs`) explicitly.

## Testing Guidelines

- Always run a compile for `esp8266:esp8266:nodemcu`.
- After flashing, verify `GET /` and `GET /update` load.
- Keypad wiring: `SDA=D2`, `SCL=D1`, power **3.3V**, I2C address auto-detects `0x20..0x27`.
- Debug auto-disables after 60s; publish `debug` to `pingpong/<MAC>/fxcmd` to re-enable for 1 minute.

## Commit & Pull Request Guidelines

- Keep commit messages short and imperative; add a scope prefix when helpful (`MQTT:`, `Keypad:`, `UI:`).
- PRs must include: target board, wiring/pins, both broker endpoints/ports, and how you validated (commands + observed MQTT lines).

## Security & Configuration Tips

- The web UI stores WiFi/MQTT settings in EEPROM; don’t commit real credentials.
- OTA `/update` is unauthenticated by design; don’t expose devices on untrusted networks.

