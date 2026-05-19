#ifndef DONGUTEC_KEYPAD_MCP23008_H
#define DONGUTEC_KEYPAD_MCP23008_H

#include <Arduino.h>
#include <Wire.h>

// Minimal driver for the Dongutec keypad interface board (MCP23008).
// Scans a 4x4 keypad by alternating which nibble is input vs output,
// matching the approach in gerph/dongutec-keypad-mcp23008-example.
class DongutecKeypadMcp23008 {
public:
  explicit DongutecKeypadMcp23008(uint8_t i2c_addr = 0x27);

  // Returns true if the device ACKs and basic configuration succeeds.
  bool begin(TwoWire& wire = Wire);

  // Returns:
  // -  0..15 : key index (layout depends on wiring/orientation)
  // - -1     : no key detected
  // - -2     : I2C error
  // - -3     : multi-key / ambiguous matrix state
  int8_t readKeyIndex();

  // Diagnostic multi-key scan.
  // key_mask uses key index bits 0..15 (same indices as readKeyIndex).
  // row_mask and col_mask use normalized 4-bit row/column sets.
  // ghost_risk is true when multiple rows and multiple columns are active.
  bool readKeyMask(uint16_t* key_mask,
                   uint8_t* row_mask = nullptr,
                   uint8_t* col_mask = nullptr,
                   bool* ghost_risk = nullptr);

private:
  bool writeReg(uint8_t reg, uint8_t value);
  bool readReg(uint8_t reg, uint8_t* out);

  uint8_t _addr;
  TwoWire* _wire = nullptr;
  int8_t _lookup[256];
};

#endif  // DONGUTEC_KEYPAD_MCP23008_H
