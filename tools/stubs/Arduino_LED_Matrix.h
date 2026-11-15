/*
 * Stub Arduino_LED_Matrix.h for host-side syntax checking ONLY.
 *
 * Transcribed from the REAL header shipped with the arduino:zephyr core 1.1.0 at
 *   ~/.arduino15/packages/arduino/hardware/zephyr/1.1.0/
 *       libraries/Arduino_LED_Matrix/src/Arduino_LED_Matrix.h
 * (read off the board, not guessed). Apache-2.0, Arduino s.r.l.
 *
 * THREE things about the real header that a from-memory stub gets wrong, and
 * which each caused or hid a real build failure:
 *
 * 1. It declares an `extern "C"` GLOBAL API: matrixBegin, matrixEnd, matrixPlay,
 *    matrixSetGrayscaleBits, matrixGrayscaleWrite, matrixWrite. Any sketch that
 *    defines its own `matrixBegin()` collides with C linkage and fails to build.
 *    This is why every matrix helper in this project is prefixed `ptMatrix`.
 *
 * 2. `draw()` takes `const uint8_t *`, NOT `uint8_t *`. An earlier version of
 *    this stub declared it non-const on the strength of a third-party blog post,
 *    which would have masked a const-correctness regression.
 *
 * 3. The file ends with `typedef Arduino_LED_Matrix ArduinoLEDMatrix;`, so BOTH
 *    spellings compile. Earlier project notes claimed only the underscored name
 *    was valid; that was wrong and is corrected here.
 *
 * It also defines a global `static inline uint32_t reverse(uint32_t)` and pulls
 * in <cmsis_core.h>; `reverse` is a plausible collision name, so it is declared
 * here too. ArduinoGraphics integration is omitted: it is conditional on
 * `__has_include("ArduinoGraphics.h")` and is tracked as a VERIFY item instead.
 */

#ifndef STUB_ARDUINO_LED_MATRIX_H
#define STUB_ARDUINO_LED_MATRIX_H

#include <Arduino.h>

/* 8 rows x 13 columns = 104 individually controllable blue LEDs, driven by the
 * STM32U585. Not reachable from the Linux/Python side. */
#define LED_MATRIX_ROWS    8
#define LED_MATRIX_COLS    13
#define LED_MATRIX_CELLS   (LED_MATRIX_ROWS * LED_MATRIX_COLS)

/* ── The real header's extern "C" global API ────────────────────────────────
 * Declared with C linkage exactly as upstream does, so that a sketch defining
 * any of these names fails here the same way it fails on the board. */
extern "C" {
void matrixBegin(void);
void matrixEnd(void);
void matrixPlay(const uint8_t *buf, uint32_t len);
void matrixSetGrayscaleBits(uint8_t _max);
void matrixGrayscaleWrite(const uint8_t *buf);
void matrixWrite(const uint32_t *buf);
}

/* Upstream defines this as a file-scope static inline; reproduced so the name is
 * visibly taken. */
static inline uint32_t reverse(uint32_t x) { return x; }

class Arduino_LED_Matrix {
public:
  Arduino_LED_Matrix() {}

  void autoscroll(uint32_t interval_ms) { (void)interval_ms; }

  /* Returns int upstream (always 1), not void. */
  int  begin(void) { return 1; }
  void end(void) {}

  /* The IRQ routine supports 8 levels (3 bits); conversion tools usually emit
   * 256 levels (8 bits). The largest value in the framebuffer should be
   * 2^max_grayscale_bits - 1, i.e. 7 or 255. */
  void setGrayscaleBits(uint8_t max) { (void)max; }

  void playVideo(const uint8_t *buf, uint32_t len) { (void)buf; (void)len; }

  /* CONST pointer. This is the 104-byte grayscale path used by the official
   * UNO Q manual examples. */
  void draw(const uint8_t *buf) { (void)buf; }

  void next(void) {}
  void loadFrame(const uint32_t buffer[4]) { (void)buffer; }
  void renderFrame(uint8_t frameNumber) { (void)frameNumber; }
  void playSequence(bool loop = false) { (void)loop; }
  bool sequenceDone(void) { return false; }

  static void loadPixelsToBuffer(uint8_t *arr, size_t size, uint32_t *dst) {
    (void)arr; (void)size; (void)dst;
  }
  void loadPixels(uint8_t *arr, size_t size) { (void)arr; (void)size; }
  void loadWrapper(const uint32_t frames[][5], uint32_t howMany) {
    (void)frames; (void)howMany;
  }
  void setCallback(void (*callBack)(void)) { (void)callBack; }
  void clear(void) {}

  /* Present only when ArduinoGraphics.h is available upstream. Declared here so
   * code using it syntax-checks; its availability on the board is a VERIFY item. */
  void set(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    (void)x; (void)y; (void)r; (void)g; (void)b;
  }
};

/* Upstream: "For backwards compatibility, also define the class name without
 * underscores". Both spellings are valid on this core. */
typedef Arduino_LED_Matrix ArduinoLEDMatrix;

#endif /* STUB_ARDUINO_LED_MATRIX_H */
