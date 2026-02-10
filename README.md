Circuit board:
https://oshwlab.com/s53zo/pingpong-lucke

## Sketches in this repo

Current firmware (root):
- `pingpong.FX_LEDs.ino` (main code)
- `antenna_mqtt_handler.cpp` / `antenna_mqtt_handler.h`

Legacy firmware:
- `legacy/pingpong.FX_LEDs.ino` (older LED-only MQTT controller)

Note for Arduino tooling:
- `pingpong-status-LED.ino` is an empty entrypoint so the sketch filename matches the folder name (required by `arduino-cli`).

## Current firmware summary (root)
This firmware turns an ESP8266 (NodeMCU) into:
- WiFi + MQTT device with a small config web UI and OTA update page
- WS2812FX LED status/effects controller (`D3`, 4 LEDs)
- MatriGS MQTT client that tracks band/antenna state and can switch antennas via MQTT commands
- USB-serial command interface (single digit `0-9` to select an antenna, plus `debug`/`stop`)

### Web UI + OTA
- `GET /` config + status page
- `POST /save` saves WiFi/MQTT/station config to EEPROM and reboots
- `GET /update` OTA upload form (`ESP8266HTTPUpdateServer` handles the POST)

### MQTT topics (current firmware)
MAC-based LED control:
- Subscribe: `pingpong/<MAC>/fxcmd`
- Publish: `pingpong/<MAC>/fxresp`
- Publish: `pingpong/<MAC>/debug`

MatriGS integration (station-based):
- Subscribe: `matrigs/0/sta/<station>/available`
- Subscribe: `matrigs/0/dt/RTX/d/<station>`
- Subscribe: `matrigs/0/sta/<station>/b/#`

### LED commands (publish to `pingpong/<MAC>/fxcmd`)
- `stop`
- `segment,<pixel>,<mode>,<brightness>,<speed>,<RRGGBB>`
- `<mode>,<brightness>,<speed>,<hexcolor>`

See https://github.com/kitesurfer1404/WS2812FX for effect/mode numbers.
