/*
 * PhishTap Lite — production firmware (Stage 1b)
 *
 * Non-blocking driver for the onboard 8x13 blue LED matrix: static labels,
 * scrolling text and state animations.
 *
 * NAMING: every helper is prefixed `ptMatrix`, never bare `matrix`.
 *
 * This is a board-verified constraint, not a style choice. The core's
 * Arduino_LED_Matrix.h declares an extern "C" global API — matrixBegin,
 * matrixEnd, matrixPlay, matrixSetGrayscaleBits, matrixGrayscaleWrite,
 * matrixWrite — plus a global reverse(). Defining our own matrixBegin() produced
 * this on real hardware:
 *
 *   error: conflicting declaration of 'void matrixBegin()' with 'C' linkage
 *   note: previous declaration with 'C++' linkage
 *
 * A C-linkage declaration and a C++-linkage definition of the same name cannot
 * coexist, so the entire build dies. The pt prefix removes the whole class of
 * collision, including names the library might add in a future release.
 *
 * PROCEDURAL ANIMATION INSTEAD OF FRAME TABLES
 * docs/ARCHITECTURE.md §5 specifies "static frame tables with no per-frame
 * allocation". The load-bearing requirement is the second half, and this module
 * meets it by computing each frame from a step counter instead of storing frames.
 * One 8x13 frame is 104 bytes, so a modest eight-frame animation would cost
 * 832 bytes of flash for something a two-line loop draws identically. Procedural
 * rendering allocates nothing, costs a few hundred bytes of code, and is easier
 * to adjust when the animation looks wrong on the actual panel.
 *
 * The framebuffer itself IS a static table, reused for every frame.
 */

#ifndef PT_MATRIX_H
#define PT_MATRIX_H

#include <Arduino.h>
#include <Arduino_LED_Matrix.h>
#include <string.h>

#include "pt_font.h"
#include "pt_config.h"

#define PT_MX_ROWS  8
#define PT_MX_COLS  13
#define PT_MX_CELLS (PT_MX_ROWS * PT_MX_COLS)   /* 104 */

/* 8-bit grayscale: values are 0..255. The panel natively supports 3 bits (8
 * levels) and the library maps the wider range down, so writing 255 simply means
 * "full brightness" and the drawing code stays free of scaling arithmetic. */
#define PT_MX_ON  255
#define PT_MX_OFF 0

/* Longest scrollable string, in panel columns: 5 columns per glyph plus a
 * 1-column spacer. 96 columns is 16 characters, which covers every message this
 * firmware displays. Longer input is truncated rather than rejected, because a
 * slightly cut-off message still communicates and an error path here would be
 * worse than the truncation. */
#define PT_SCROLL_MAX_COLS 96
#define PT_SCROLL_MAX_TEXT 24

typedef enum : uint8_t {
  PT_MX_MODE_OFF = 0,
  PT_MX_MODE_STATIC,   /* a fixed one or two character label */
  PT_MX_MODE_SCROLL,   /* text scrolling right to left       */
  PT_MX_MODE_ANIM      /* a procedural state animation       */
} PtMatrixMode;

typedef enum : uint8_t {
  PT_MXA_NONE = 0,
  PT_MXA_SWEEP,        /* a bright column sweeps left and right  */
  PT_MXA_PULSE,        /* the border pulses                      */
  PT_MXA_BLINK,        /* the whole panel blinks, for link-lost  */
  PT_MXA_SPARKLE       /* pseudo-random dots, attract mode       */
} PtMatrixAnim;

/* The class is Arduino_LED_Matrix. The header ends with
 * `typedef Arduino_LED_Matrix ArduinoLEDMatrix;`, so both spellings compile on
 * this core. */
static Arduino_LED_Matrix s_matrix;

/* One framebuffer, indexed [row * 13 + col], holding one brightness byte per
 * LED. draw() takes `const uint8_t *` on this core, but the buffer is mutable
 * because the drawing helpers write into it in place. */
static uint8_t s_frame[PT_MX_CELLS];

static struct {
  PtMatrixMode mode;
  PtMatrixAnim anim;
  uint32_t     lastTick;
  /* Scroll state. */
  uint8_t      cols[PT_SCROLL_MAX_COLS];
  int          scrollTotal;
  int          scrollOffset;
  bool         scrollLoop;
  bool         scrollDone;
  /* Animation state. */
  uint16_t     animStep;
} s_mx;

/* ── Low level ──────────────────────────────────────────────────────────── */

/* Returns true if the panel initialised. begin() returns an int on this core, so
 * a failure is detectable and is reported as PT_FAULT_MATRIX rather than
 * silently producing a console with a dead display. */
inline bool ptMatrixBegin(void) {
  int rc = s_matrix.begin();
  s_matrix.setGrayscaleBits(8);
  memset(s_frame, 0, sizeof(s_frame));
  memset(&s_mx, 0, sizeof(s_mx));
  s_matrix.draw(s_frame);
  return (rc != 0);
}

inline void ptMatrixClearBuffer(void) {
  memset(s_frame, 0, sizeof(s_frame));
}

/* Push the buffer to the panel. Kept separate from the drawing helpers so a
 * frame can be composed from several calls before one refresh, which avoids the
 * visible tearing of drawing pixel by pixel. */
inline void ptMatrixFlush(void) {
  s_matrix.draw(s_frame);
}

inline void ptMatrixSetPixel(uint8_t row, uint8_t col, uint8_t value) {
  if (row >= PT_MX_ROWS || col >= PT_MX_COLS) return;
  s_frame[(row * PT_MX_COLS) + col] = value;
}

/* Paint one glyph at a column offset that may be negative or off-panel;
 * out-of-range columns are clipped. The scroller reuses this for every frame
 * without needing a separate clipping pass. */
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

/* ── Static label ─────────────────────────────────────────────────────────
 * Shows up to two characters, roughly centred. The panel is 13 columns wide and
 * a glyph is 5, so two glyphs at offsets 1 and 7 leave a one-column margin each
 * side and a one-column gap between them.
 */
inline void ptMatrixShowText2(const char* text) {
  ptMatrixClearBuffer();
  if (text == NULL || text[0] == '\0') {
    s_mx.mode = PT_MX_MODE_OFF;
    ptMatrixFlush();
    return;
  }
  ptMatrixDrawGlyphAt(text[0], 1, 0);
  if (text[1] != '\0') ptMatrixDrawGlyphAt(text[1], 7, 0);
  s_mx.mode     = PT_MX_MODE_STATIC;
  s_mx.lastTick = millis();
  ptMatrixFlush();
}

/* ── Scrolling text ───────────────────────────────────────────────────────
 * The glyph data is row-major but scrolling is column-major, so the text is
 * transposed ONCE into a run of column bytes (bit r of a column byte is the
 * pixel at row r). Doing that up front keeps the per-frame work down to a slice
 * and a copy, which matters because a frame is pushed every PT_MATRIX_TICK_MS
 * and a slow transcode would show up as visible stutter.
 */
inline void ptMatrixScrollStart(const char* text, bool loop) {
  if (text == NULL) return;

  memset(s_mx.cols, 0, sizeof(s_mx.cols));
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
      s_mx.cols[total++] = colByte;
    }
    s_mx.cols[total++] = 0x00;   /* inter-glyph spacer */
  }

  s_mx.scrollTotal  = total;
  s_mx.scrollOffset = PT_MX_COLS;   /* start fully off the right edge */
  s_mx.scrollLoop   = loop;
  s_mx.scrollDone   = false;
  s_mx.mode         = PT_MX_MODE_SCROLL;
  s_mx.lastTick     = millis();
}

/* Render the current scroll offset into the framebuffer. */
inline void ptMatrixScrollRender(void) {
  ptMatrixClearBuffer();
  for (uint8_t x = 0; x < PT_MX_COLS; ++x) {
    int src = (int)x - s_mx.scrollOffset;
    uint8_t colByte = (src >= 0 && src < s_mx.scrollTotal) ? s_mx.cols[src] : 0x00;
    for (uint8_t r = 0; r < PT_MX_ROWS; ++r) {
      s_frame[(r * PT_MX_COLS) + x] = ((colByte >> r) & 0x01) ? PT_MX_ON : PT_MX_OFF;
    }
  }
  ptMatrixFlush();
}

inline bool ptMatrixScrollDone(void) {
  return s_mx.scrollDone;
}

/* ── Procedural animations ──────────────────────────────────────────────── */

inline void ptMatrixAnimStart(PtMatrixAnim a) {
  s_mx.anim      = a;
  s_mx.animStep  = 0;
  s_mx.mode      = PT_MX_MODE_ANIM;
  s_mx.lastTick  = millis();
}

/* A tiny deterministic pseudo-random generator for SPARKLE.
 *
 * Deliberately NOT random(): seeding from an unseeded random() would produce the
 * same pattern every boot, and seeding from analogRead on a pin that is not
 * connected is a common source of subtly non-random behaviour. An xorshift over a
 * fixed seed is reproducible, which means the attract animation looks the same on
 * every console and a report of "the screen does something odd" can be
 * reproduced exactly. */
static uint32_t s_mxRand = 0x2545F491u;
inline uint32_t ptMatrixRand(void) {
  s_mxRand ^= s_mxRand << 13;
  s_mxRand ^= s_mxRand >> 17;
  s_mxRand ^= s_mxRand << 5;
  return s_mxRand;
}

inline void ptMatrixAnimRender(void) {
  ptMatrixClearBuffer();
  uint16_t step = s_mx.animStep;

  switch (s_mx.anim) {
    case PT_MXA_SWEEP: {
      /* A two-column bright band bouncing between the edges. Period is
       * 2 * (COLS - 2) so the band reverses at each end without dwelling. */
      uint16_t period = (uint16_t)(2 * (PT_MX_COLS - 2));
      uint16_t pos    = (uint16_t)(step % period);
      int col = (pos < (PT_MX_COLS - 2))
                  ? (int)pos
                  : (int)(period - pos);
      for (uint8_t r = 0; r < PT_MX_ROWS; ++r) {
        ptMatrixSetPixel(r, (uint8_t)col, PT_MX_ON);
        if (col + 1 < PT_MX_COLS) ptMatrixSetPixel(r, (uint8_t)(col + 1), PT_MX_ON);
      }
      break;
    }
    case PT_MXA_PULSE: {
      /* The border, on for three steps then off for three. */
      bool on = ((step / 3) % 2) == 0;
      if (on) {
        for (uint8_t c = 0; c < PT_MX_COLS; ++c) {
          ptMatrixSetPixel(0, c, PT_MX_ON);
          ptMatrixSetPixel(PT_MX_ROWS - 1, c, PT_MX_ON);
        }
        for (uint8_t r = 0; r < PT_MX_ROWS; ++r) {
          ptMatrixSetPixel(r, 0, PT_MX_ON);
          ptMatrixSetPixel(r, PT_MX_COLS - 1, PT_MX_ON);
        }
      }
      break;
    }
    case PT_MXA_BLINK: {
      /* Whole panel, used for link-lost so it is unmistakable from a distance. */
      bool on = ((step / 4) % 2) == 0;
      if (on) {
        for (uint16_t i = 0; i < PT_MX_CELLS; ++i) s_frame[i] = PT_MX_ON;
      }
      break;
    }
    case PT_MXA_SPARKLE: {
      /* Eight random lit pixels per frame. Sparse enough to read as an attract
       * animation rather than as noise or as a fault. */
      for (uint8_t i = 0; i < 8; ++i) {
        uint32_t v = ptMatrixRand();
        ptMatrixSetPixel((uint8_t)(v % PT_MX_ROWS),
                         (uint8_t)((v >> 8) % PT_MX_COLS), PT_MX_ON);
      }
      break;
    }
    default:
      break;
  }
  ptMatrixFlush();
}

/* ── Control ────────────────────────────────────────────────────────────── */

inline void ptMatrixOff(void) {
  s_mx.mode = PT_MX_MODE_OFF;
  s_mx.anim = PT_MXA_NONE;
  ptMatrixClearBuffer();
  ptMatrixFlush();
}

/* Light every pixel. Used by the boot selftest as a single unmistakable
 * confirmation that the whole panel is driven, which catches dead pixels and a
 * panel that is not being scanned at all. Sets mode to STATIC so the tick
 * function leaves it alone rather than overwriting it on the next frame. */
inline void ptMatrixAllOn(void) {
  for (uint16_t i = 0; i < PT_MX_CELLS; ++i) s_frame[i] = PT_MX_ON;
  s_mx.mode     = PT_MX_MODE_STATIC;
  s_mx.lastTick = millis();
  ptMatrixFlush();
}

inline PtMatrixMode ptMatrixGetMode(void) {
  return s_mx.mode;
}

/* Advance whichever mode is active. Called once per loop() iteration; returns
 * immediately unless a frame interval has elapsed, so the idle cost is one
 * subtraction and one comparison.
 *
 * `now` is passed in rather than read here so that every engine in a single
 * loop() iteration sees the SAME timestamp. Reading millis() independently in
 * each engine lets them disagree by a millisecond, which is harmless most of the
 * time and maddening to reason about when it is not. */
inline void ptMatrixTick(uint32_t now) {
  if (s_mx.mode == PT_MX_MODE_OFF || s_mx.mode == PT_MX_MODE_STATIC) return;
  if ((uint32_t)(now - s_mx.lastTick) < PT_MATRIX_TICK_MS) return;
  s_mx.lastTick = now;

  if (s_mx.mode == PT_MX_MODE_SCROLL) {
    ptMatrixScrollRender();
    s_mx.scrollOffset--;
    if (s_mx.scrollOffset < -s_mx.scrollTotal) {
      if (s_mx.scrollLoop) {
        s_mx.scrollOffset = PT_MX_COLS;
      } else {
        s_mx.scrollDone = true;
        s_mx.mode       = PT_MX_MODE_OFF;
        ptMatrixClearBuffer();
        ptMatrixFlush();
      }
    }
  } else if (s_mx.mode == PT_MX_MODE_ANIM) {
    ptMatrixAnimRender();
    s_mx.animStep++;
  }
}

#endif /* PT_MATRIX_H */
