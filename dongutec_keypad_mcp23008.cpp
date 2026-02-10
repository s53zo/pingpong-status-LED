#include "dongutec_keypad_mcp23008.h"

namespace {
// MCP23008 register addresses (see Microchip datasheet 21919E).
constexpr uint8_t REG_IODIR = 0x00;
constexpr uint8_t REG_IPOL  = 0x01;
constexpr uint8_t REG_GPPU  = 0x06;
constexpr uint8_t REG_GPIO  = 0x09;
constexpr uint8_t REG_OLAT  = 0x0A;
}  // namespace

DongutecKeypadMcp23008::DongutecKeypadMcp23008(uint8_t i2c_addr) : _addr(i2c_addr)
{
  for (int i = 0; i < 256; ++i) _lookup[i] = -1;

  // Same lookup-table construction as the reference Python example.
  for (int key = 0; key < 16; ++key) {
    uint8_t row_mask = 1u << (3 - (key & 3));        // 0x8,0x4,0x2,0x1
    uint8_t col_mask = 16u << (3 - (key >> 2));      // 0x80,0x40,0x20,0x10
    _lookup[row_mask | col_mask] = (int8_t)key;
  }
}

bool DongutecKeypadMcp23008::begin(TwoWire& wire)
{
  _wire = &wire;

  // Probe for ACK.
  _wire->beginTransmission(_addr);
  if (_wire->endTransmission() != 0) return false;

  // Invert inputs and enable pull-ups, to make pressed keys read as 1s.
  if (!writeReg(REG_IPOL, 0xFF)) return false;
  if (!writeReg(REG_GPPU, 0xFF)) return false;

  // Ensure any outputs we enable during scanning will be driven low.
  if (!writeReg(REG_OLAT, 0x00)) return false;

  return true;
}

int8_t DongutecKeypadMcp23008::readKeyIndex()
{
  if (!_wire) return -2;

  uint8_t v_hi = 0;
  uint8_t v_lo = 0;

  // Phase 1: upper nibble inputs, lower nibble outputs.
  if (!writeReg(REG_IODIR, 0xF0)) return -2;
  delayMicroseconds(50);
  if (!readReg(REG_GPIO, &v_hi)) return -2;

  // Phase 2: lower nibble inputs, upper nibble outputs.
  if (!writeReg(REG_IODIR, 0x0F)) return -2;
  delayMicroseconds(50);
  if (!readReg(REG_GPIO, &v_lo)) return -2;

  uint8_t combined = v_hi | v_lo;
  return _lookup[combined];  // -1 if nothing/unknown/multi-key
}

bool DongutecKeypadMcp23008::writeReg(uint8_t reg, uint8_t value)
{
  _wire->beginTransmission(_addr);
  _wire->write(reg);
  _wire->write(value);
  return _wire->endTransmission() == 0;
}

bool DongutecKeypadMcp23008::readReg(uint8_t reg, uint8_t* out)
{
  if (!out) return false;

  _wire->beginTransmission(_addr);
  _wire->write(reg);
  if (_wire->endTransmission(false) != 0) return false;  // repeated start

  const uint8_t n = _wire->requestFrom((int)_addr, 1);
  if (n != 1) return false;
  *out = (uint8_t)_wire->read();
  return true;
}

