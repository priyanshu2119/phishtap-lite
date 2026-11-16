/*
 * Stub Arduino.h for host-side syntax checking ONLY.
 *
 * This is NOT a functional Arduino implementation. It exists so that
 * tools/syntax_check.sh can run `g++ -fsyntax-only` over the sketch sources on a
 * laptop and catch real C++ errors — missing forward declarations, type
 * mismatches, unknown identifiers — before they cost a flash cycle on the board.
 *
 * It deliberately mirrors the API SURFACE of the real headers, not their
 * behaviour. Every symbol declared here was checked against the official UNO Q
 * documentation or the upstream library source it stands in for.
 *
 * <math.h> is included on purpose. The real toolchain pulls it in transitively,
 * and doing the same is what surfaces name collisions with the C library —
 * which is exactly how logf() was caught.
 */

#ifndef STUB_ARDUINO_H
#define STUB_ARDUINO_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef uint8_t  byte;
typedef bool     boolean;
typedef uint16_t word;

/* ── Pin levels and modes ───────────────────────────────────────────────────
 * Values match the real core's api/Common.h. Both the constants AND the enum
 * types have to exist, because sketch code passes PinMode around as a parameter
 * type rather than as a bare integer.
 *
 * INPUT_PULLDOWN was verified present on the board's core 1.1.0 before being
 * relied upon: cores/arduino/api/Common.h defines it as 0x3, and
 * wiring_digital.cpp maps it to GPIO_PULL_DOWN, so it configures a real internal
 * pull-down rather than silently doing nothing.
 */
enum PinMode : uint8_t {
  INPUT          = 0x0,
  OUTPUT         = 0x1,
  INPUT_PULLUP   = 0x2,
  INPUT_PULLDOWN = 0x3,
  INPUT_ANALOG   = 0x4
};

enum PinStatus : uint8_t {
  LOW  = 0x0,
  HIGH = 0x1
};

#define LSBFIRST 0
#define MSBFIRST 1

#define LED_BUILTIN 22   /* UNO Q: LED_BUILTIN == LED3_R (verified) */

/* UNO Q digital header. Values are arbitrary but distinct; only their existence
 * and uniqueness matter for a syntax check. Real mapping is PB7..PB13 etc. */
#define D0  0
#define D1  1
#define D2  2
#define D3  3
#define D4  4
#define D5  5
#define D6  6
#define D7  7
#define D8  8
#define D9  9
#define D10 10
#define D11 11
#define D12 12
#define D13 13
#define D14 14
#define D15 15
#define D16 16
#define D17 17
#define D18 18
#define D19 19
#define D20 20
#define D21 21

#define A0 14
#define A1 15
#define A2 16
#define A3 17
#define A4 18
#define A5 19

/* SPI pin aliases. The real core defines these in pins_arduino.h; third-party
 * libraries such as MFRC522 reference bare `SS` in their default constructors,
 * so without these the library cannot be syntax-checked at all. */
#define SS   10
#define MOSI 11
#define MISO 12
#define SCK  13
#define SDA  20
#define SCL  21

/* Onboard MCU-controlled RGB LEDs. Verified against the official UNO Q
 * datasheet: LED3_R=PH10, LED3_G=PH11, LED3_B=PH12, LED4_R=PH13, LED4_G=PH14,
 * LED4_B=PH15, and they are ACTIVE-LOW. Contiguous from LED_BUILTIN, as the
 * official unoq-pin-toggle example relies on. */
#define LED3_R (LED_BUILTIN + 0)
#define LED3_G (LED_BUILTIN + 1)
#define LED3_B (LED_BUILTIN + 2)
#define LED4_R (LED_BUILTIN + 3)
#define LED4_G (LED_BUILTIN + 4)
#define LED4_B (LED_BUILTIN + 5)

#ifndef F_CPU
#define F_CPU 160000000L   /* STM32U585 @ 160 MHz */
#endif

/* ── Core functions ─────────────────────────────────────────────────────────
 * Signatures use the real PinMode and PinStatus types rather than bare integers,
 * so that passing a PinMode variable is type-checked the same way here as on the
 * board. */
inline void pinMode(uint8_t, PinMode) {}
inline void digitalWrite(uint8_t, PinStatus) {}
inline PinStatus digitalRead(uint8_t) { return LOW; }
inline int  analogRead(uint8_t) { return 0; }
inline void analogWrite(uint8_t, int) {}
inline void analogWriteResolution(int) {}
inline void analogReadResolution(int) {}

inline unsigned long millis(void) { return 0; }
inline unsigned long micros(void) { return 0; }
inline void delay(unsigned long) {}
inline void delayMicroseconds(unsigned int) {}

/* Cooperative-scheduler hook. Called from busy-wait loops in many libraries
 * (MFRC522 calls it while waiting for a CRC result), so it must exist. */
inline void yield(void) {}

inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

inline long constrain(long x, long a, long b) {
  return (x < a) ? a : ((x > b) ? b : x);
}

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define abs(x)    ((x) > 0 ? (x) : -(x))
#define round(x)  ((long)((x) + 0.5))

/* ── AVR PROGMEM compatibility shims ────────────────────────────────────────
 * Present because third-party Arduino libraries use them unconditionally. On
 * ARM/Zephyr these are all no-ops; declaring them here means a library header
 * that references PROGMEM still parses.
 */
#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef PSTR
#define PSTR(s) (s)
#endif
#ifndef pgm_read_byte
#define pgm_read_byte(addr)  (*(const uint8_t *)(addr))
#endif
#ifndef pgm_read_word
#define pgm_read_word(addr)  (*(const uint16_t *)(addr))
#endif
#ifndef pgm_read_dword
#define pgm_read_dword(addr) (*(const uint32_t *)(addr))
#endif
#ifndef pgm_read_ptr
#define pgm_read_ptr(addr)   (*(const void * const *)(addr))
#endif

/*
 * __FlashStringHelper is an AVR PROGMEM abstraction with no meaning on ARM.
 * Declared here so signatures that return it still compile — and so that any
 * sketch code which tries to USE one is visible as a type mismatch rather than
 * silently passing. The selftest deliberately avoids it.
 */
class __FlashStringHelper;

/* F() must yield a __FlashStringHelper*, not a const char*. Libraries pass the
 * result straight to Serial.print/println, so if F() returned const char* the
 * overload set would resolve differently here than on the board and a real
 * mismatch could be masked. */
#define F(string_literal) (reinterpret_cast<const __FlashStringHelper *>(string_literal))

/* ── String and Stream ──────────────────────────────────────────────────────
 * Deliberately minimal. The project rule is to avoid heap String in hot paths,
 * so only the operations actually used are modelled.
 */
class String {
public:
  String() {}
  String(const char *) {}
  String(int) {}
  String(unsigned int) {}
  String(long) {}
  String(unsigned long) {}
  const char *c_str(void) const { return ""; }
  unsigned int length(void) const { return 0; }
  String &operator=(const String &) { return *this; }
  String &operator+=(const String &) { return *this; }
  bool operator==(const String &) const { return false; }
  bool operator!=(const String &) const { return true; }
};

class Print {
public:
  virtual ~Print() {}
  size_t print(const char *) { return 0; }
  size_t print(const String &) { return 0; }
  size_t print(const __FlashStringHelper *) { return 0; }
  size_t print(char) { return 0; }
  size_t print(int, int = 10) { return 0; }
  size_t print(unsigned int, int = 10) { return 0; }
  size_t print(long, int = 10) { return 0; }
  size_t print(unsigned long, int = 10) { return 0; }
  size_t print(double, int = 2) { return 0; }
  size_t println(void) { return 0; }
  size_t println(const char *) { return 0; }
  size_t println(const String &) { return 0; }
  size_t println(const __FlashStringHelper *) { return 0; }
  size_t println(char) { return 0; }
  size_t println(int, int = 10) { return 0; }
  size_t println(unsigned int, int = 10) { return 0; }
  size_t println(long, int = 10) { return 0; }
  size_t println(unsigned long, int = 10) { return 0; }
  size_t println(double, int = 2) { return 0; }
  size_t write(uint8_t) { return 0; }
  size_t write(const uint8_t *, size_t) { return 0; }
};

class Stream : public Print {
public:
  virtual ~Stream() {}
  int available(void) { return 0; }
  int read(void) { return -1; }
  int peek(void) { return -1; }
  void flush(void) {}
};

class HardwareSerial : public Stream {
public:
  void begin(unsigned long) {}
  void begin(unsigned long, uint8_t) {}
  void end(void) {}
  operator bool() const { return true; }
};

extern HardwareSerial Serial;
extern HardwareSerial Serial1;

#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2

#endif /* STUB_ARDUINO_H */
