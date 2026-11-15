/*
 * Stub SPI.h for host-side syntax checking ONLY.
 *
 * Surface matches the Arduino SPI API as used by the official UNO Q SPI example
 * and by the MFRC522 library.
 */

#ifndef STUB_SPI_H
#define STUB_SPI_H

#include <Arduino.h>

#define SPI_MODE0 0x00
#define SPI_MODE1 0x04
#define SPI_MODE2 0x08
#define SPI_MODE3 0x0C

class SPISettings {
public:
  SPISettings() : _clock(4000000), _order(MSBFIRST), _mode(SPI_MODE0) {}
  SPISettings(uint32_t clock, uint8_t bitOrder, uint8_t dataMode)
      : _clock(clock), _order(bitOrder), _mode(dataMode) {}

private:
  uint32_t _clock;
  uint8_t  _order;
  uint8_t  _mode;
};

class SPIClass {
public:
  /* The official UNO Q SPI example calls SPI.begin() with no arguments after
   * driving SS high manually, so the no-arg form is the one that must parse. */
  void begin(void) {}
  void end(void) {}
  void beginTransaction(SPISettings) {}
  void endTransaction(void) {}
  uint8_t transfer(uint8_t data) { return data; }
  uint16_t transfer16(uint16_t data) { return data; }
  void transfer(void *buf, size_t count) { (void)buf; (void)count; }
  void setBitOrder(uint8_t) {}
  void setDataMode(uint8_t) {}
  void setClockDivider(uint32_t) {}
};

extern SPIClass SPI;

#endif /* STUB_SPI_H */
