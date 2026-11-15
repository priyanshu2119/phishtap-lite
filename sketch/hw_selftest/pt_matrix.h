/*
 * PhishTap Lite — hw_selftest
 *
 * Thin wrapper around the onboard 8x13 blue LED matrix.
 */

#ifndef PT_MATRIX_H
#define PT_MATRIX_H

#include <Arduino.h>
#include <Arduino_LED_Matrix.h>
#include <string.h>

#include "pt_font.h"

/*
 * Naming: every helper here is prefixed `ptMatrix`, never bare `matrix`.
 *
 * WHY — this is a real, board-verified collision. The core's
 * Arduino_LED_Matrix.h declares an extern "C" global API:
 *
 *     extern "C" {
 *     void matrixBegin(void);
 *     void matrixEnd(void);
 *     void matrixPlay(const uint8_t *buf, uint32_t len);
 *     void matrixSetGrayscaleBits(uint8_t _max);
 *     void matrixGrayscaleWrite(const uint8_t *buf);
 *     void matrixWrite(const uint32_t *buf);
 *     };
 *
 * Defining our own `matrixBegin()` produced, on the board:
 *   error: conflicting declaration of 'void matrixBegin()' with 'C' linkage
 *   note: previous declaration with 'C++' linkage
 * A C-linkage declaration and a C++-linkage definition of the same name cannot
 * coexist, so the whole build dies. The `pt` prefix removes the entire class of
 * collision, including names the library might add later.
 *
 * The class itself is `Arduino_LED_Matrix`. The header also ends with
 * `typedef Arduino_LED_Matrix ArduinoLEDMatrix;`, so both spellings compile on
 * this core — earlier project notes claiming otherwise were wrong.
 */

#define PT_MX_ROWS 8
#define PT_MX_COLS 13
#define PT_MX_CELLS (PT_MX_ROWS * PT_MX_COLS)   /* 104 */

/*
 * One instance, file-scope static.
 *
 * WHY static and not a bare global: every module in this selftest is a header
 * included exactly once from the .ino, so there is a single translation unit and
 * a single instance either way. Marking it static makes that intent explicit and
 * keeps the symbol from leaking if the layout ever changes to separate .cpp
 * files (which Stage 1b will do once App Lab's handling of .cpp is confirmed).
 */
static Arduino_LED_Matrix s_matrix;

/*
 * The frame buffer is indexed [row * 13 + col] and holds one brightness byte
 * per LED.
 *
 * draw() takes `const uint8_t *` on this core, so a const buffer would also be
 * accepted. The buffer is kept mutable anyway because the drawing helpers write
 * into it in place, and because relying on constness here would buy nothing.
 */
static uint8_t s_frame[PT_MX_CELLS];

/* 8-bit grayscale means values are 0..255. The panel's IRQ routine natively
 * supports 3 bits (8 levels); asking for 8 lets the library map the 0..255
 * range down for us, so writing 255 simply means "full brightness" and the
 * drawing code stays free of scaling arithmetic. */
#define PT_MX_ON  255
#define PT_MX_OFF 0

/* Longest scrollable string: 5 columns per glyph plus a 1-column spacer. */
#define PT_SCROLL_MAX_COLS 96

inline void ptMatrixBegin(void) {
  s_matrix.begin();
  s_matrix.setGrayscaleBits(8);
  memset(s_frame, 0, sizeof(s_frame));
  s_matrix.draw(s_frame);
}

inline void ptMatrixClearBuffer(void) {
  memset(s_frame, 0, sizeof(s_frame));
}

/* Push the buffer to the panel. Kept separate from the drawing helpers so a
 * frame can be composed from several calls before a single refresh, which avoids
 * the visible tearing of drawing pixel by pixel. */
inline void ptMatrixFlush(void) {
  s_matrix.draw(s_frame);
}

inline void ptMatrixClear(void) {
  ptMatrixClearBuffer();
  ptMatrixFlush();
}

inline void ptMatrixSetPixel(uint8_t row, uint8_t col, uint8_t value) {
  if (row >= PT_MX_ROWS || col >= PT_MX_COLS) return;
  s_frame[(row * PT_MX_COLS) + col] = value;
}

/* Paint one glyph into the buffer at a column offset that may be negative or
 * off-panel; out-of-range columns are clipped. That lets the scroller reuse this
 * for every frame without a separate clipping pass. */
inline void ptMatrixDrawGlyphAt(char c, int colOffset, uint8_t rowOffset) {
  const uint8_t* glyph = ptGlyphFor(c);
  for (uint8_t r = 0; r < PT_GLYPH_ROWS; ++r) {
    uint8_t bits = (glyph != NULL) ? glyph[r] : 0x00;
    for (uint8_t col = 0; col < PT_GLYPH_COLS; ++col) {
      int x = colOffset + (int)col;
      if (x < 0 || x >= PT_MX_COLS) continue;
      bool on = ((bits >> (4 - col)) & 0x01) != 0;
      ptMatrixSetPixel((uint8_t)(rowOffset + r), (uint8_t)x,
                       on ? PT_MX_ON : PT_MX_OFF);
    }
  }
}

/* Show up to two characters, roughly centred. Used for short status codes. */
inline void ptMatrixShowText2(const char* text) {
  ptMatrixClearBuffer();
  if (text == NULL || text[0] == '\0') { ptMatrixFlush(); return; }
  ptMatrixDrawGlyphAt(text[0], 1, 0);
  if (text[1] != '\0') ptMatrixDrawGlyphAt(text[1], 7, 0);
  ptMatrixFlush();
}

/*
 * Scroll a string right-to-left.
 *
 * The glyph data is row-major but scrolling is column-major, so the text is
 * first transposed into a run of column bytes (bit r of a column byte is the
 * pixel at row r). Doing that once up front keeps the per-frame work down to a
 * memcpy-like slice, which matters because a frame is pushed every stepMs and a
 * slow transcode would show up as visible stutter.
 */
inline void ptMatrixScrollText(const char* text, uint16_t stepMs) {
  if (text == NULL) return;

  uint8_t cols[PT_SCROLL_MAX_COLS];
  int total = 0;

  for (const char* p = text; *p != '\0'; ++p) {
    if (total + PT_GLYPH_COLS + 1 > PT_SCROLL_MAX_COLS) break;
    const uint8_t* glyph = ptGlyphFor(*p);
    for (uint8_t c = 0; c < PT_GLYPH_COLS; ++c) {
      uint8_t colByte = 0;
      for (uint8_t r = 0; r < PT_GLYPH_ROWS; ++r) {
        uint8_t bits = (glyph != NULL) ? glyph[r] : 0x00;
        if (((bits >> (4 - c)) & 0x01) != 0) colByte |= (uint8_t)(1u << r);
      }
      cols[total++] = colByte;
    }
    cols[total++] = 0x00;   /* inter-glyph spacer */
  }

  for (int off = PT_MX_COLS; off > -total; --off) {
    ptMatrixClearBuffer();
    for (uint8_t x = 0; x < PT_MX_COLS; ++x) {
      int src = (int)x - off;
      uint8_t colByte = (src >= 0 && src < total) ? cols[src] : 0x00;
      for (uint8_t r = 0; r < PT_MX_ROWS; ++r) {
        s_frame[(r * PT_MX_COLS) + x] =
            ((colByte >> r) & 0x01) ? PT_MX_ON : PT_MX_OFF;
      }
    }
    ptMatrixFlush();
    delay(stepMs);
  }
}

/* ── Diagnostic patterns ────────────────────────────────────────────────────
 * Each pattern is chosen to expose a different class of panel fault, because a
 * matrix that "does something" is not the same as a matrix that works:
 *   allOn        - dead pixels, and whether the whole panel is driven at all
 *   checkerboard - adjacent-pixel shorts and row/column crosstalk
 *   border       - whether the outermost rows and columns are wired
 *   rowSweep     - one row at a time, so a stuck or dead ROW is obvious
 *   colSweep     - one column at a time, so a stuck or dead COLUMN is obvious
 * A panel with a broken scan line passes allOn but fails the matching sweep.
 */

inline void ptMatrixPatternAllOn(void) {
  for (uint16_t i = 0; i < PT_MX_CELLS; ++i) s_frame[i] = PT_MX_ON;
  ptMatrixFlush();
}

inline void ptMatrixPatternCheckerboard(uint8_t phase) {
  for (uint8_t r = 0; r < PT_MX_ROWS; ++r) {
    for (uint8_t c = 0; c < PT_MX_COLS; ++c) {
      bool on = (((r + c) & 0x01) == phase);
      s_frame[(r * PT_MX_COLS) + c] = on ? PT_MX_ON : PT_MX_OFF;
    }
  }
  ptMatrixFlush();
}

inline void ptMatrixPatternBorder(void) {
  ptMatrixClearBuffer();
  for (uint8_t c = 0; c < PT_MX_COLS; ++c) {
    ptMatrixSetPixel(0, c, PT_MX_ON);
    ptMatrixSetPixel(PT_MX_ROWS - 1, c, PT_MX_ON);
  }
  for (uint8_t r = 0; r < PT_MX_ROWS; ++r) {
    ptMatrixSetPixel(r, 0, PT_MX_ON);
    ptMatrixSetPixel(r, PT_MX_COLS - 1, PT_MX_ON);
  }
  ptMatrixFlush();
}

inline void ptMatrixPatternRowSweep(void) {
  for (uint8_t r = 0; r < PT_MX_ROWS; ++r) {
    ptMatrixClearBuffer();
    for (uint8_t c = 0; c < PT_MX_COLS; ++c) ptMatrixSetPixel(r, c, PT_MX_ON);
    ptMatrixFlush();
    delay(180);
  }
}

inline void ptMatrixPatternColSweep(void) {
  for (uint8_t c = 0; c < PT_MX_COLS; ++c) {
    ptMatrixClearBuffer();
    for (uint8_t r = 0; r < PT_MX_ROWS; ++r) ptMatrixSetPixel(r, c, PT_MX_ON);
    ptMatrixFlush();
    delay(140);
  }
}

#endif /* PT_MATRIX_H */
