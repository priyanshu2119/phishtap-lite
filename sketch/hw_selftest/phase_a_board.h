/*
 * PhishTap Lite — hw_selftest
 *
 * PHASE A — board-only checks. Zero jumper wires.
 *
 * Covers the two peripherals that are soldered to the board and therefore cannot
 * be mis-wired: the 8x13 LED matrix and the MCU-controlled RGB LEDs (LED3/LED4).
 * It also implicitly proves that the Monitor logging path works, because if it
 * did not you would be reading none of this.
 */

#ifndef PHASE_A_BOARD_H
#define PHASE_A_BOARD_H

#include <Arduino.h>

#include "pt_pins.h"
#include "pt_log.h"
#include "pt_matrix.h"

/* ── Onboard RGB LEDs ───────────────────────────────────────────────────────
 * LED3 and LED4 are driven by the STM32 on PH10..PH15. LED1 and LED2 belong to
 * the Linux side and are reached through /sys/class/leds, not from a sketch, so
 * they are out of scope here.
 *
 * They are ACTIVE-LOW: writing LOW lights the segment. This is the reverse of
 * the external lives LEDs in Phase C, and it is confirmed by the official
 * arduino/app-bricks-examples "unoq-pin-toggle" sketch, which writes HIGH to all
 * six segments in setup() precisely to turn them off.
 */

/*
 * Forward declaration.
 *
 * rgbBegin() calls rgbAllOff(), which is defined below it. Arduino's build only
 * auto-generates prototypes for functions defined in the main .ino file — code
 * in an included header gets none — so without this declaration the call is
 * undeclared at that point and the sketch does not compile. Every helper in this
 * project's headers that is used before its definition needs the same treatment.
 */
inline void rgbAllOff(void);

inline void rgbBegin(void) {
  pinMode(PT_RGB3_R, OUTPUT);
  pinMode(PT_RGB3_G, OUTPUT);
  pinMode(PT_RGB3_B, OUTPUT);
  pinMode(PT_RGB4_R, OUTPUT);
  pinMode(PT_RGB4_G, OUTPUT);
  pinMode(PT_RGB4_B, OUTPUT);
  rgbAllOff();
}

inline void rgbAllOff(void) {
  digitalWrite(PT_RGB3_R, PT_RGB_OFF);
  digitalWrite(PT_RGB3_G, PT_RGB_OFF);
  digitalWrite(PT_RGB3_B, PT_RGB_OFF);
  digitalWrite(PT_RGB4_R, PT_RGB_OFF);
  digitalWrite(PT_RGB4_G, PT_RGB_OFF);
  digitalWrite(PT_RGB4_B, PT_RGB_OFF);
}

inline void rgbSetLed3(bool r, bool g, bool b) {
  digitalWrite(PT_RGB3_R, r ? PT_RGB_ON : PT_RGB_OFF);
  digitalWrite(PT_RGB3_G, g ? PT_RGB_ON : PT_RGB_OFF);
  digitalWrite(PT_RGB3_B, b ? PT_RGB_ON : PT_RGB_OFF);
}

inline void rgbSetLed4(bool r, bool g, bool b) {
  digitalWrite(PT_RGB4_R, r ? PT_RGB_ON : PT_RGB_OFF);
  digitalWrite(PT_RGB4_G, g ? PT_RGB_ON : PT_RGB_OFF);
  digitalWrite(PT_RGB4_B, b ? PT_RGB_ON : PT_RGB_OFF);
}

/*
 * Run Phase A.
 *
 * Every check here is VISUAL by necessity. Neither the matrix nor an LED has a
 * readback path, so the firmware cannot observe its own output; claiming PASS
 * would be dishonest and claiming FAIL would be wrong. The operator confirms by
 * eye and the verdict is recorded as VISUAL.
 */
inline void phaseA_run(void) {
  logSection("PHASE A - BOARD ONLY (0 wires)");

  /* A1: logging. If this line is visible the Bridge + Monitor path works, which
   * is the precondition for every later phase reporting anything at all. */
  logStep("A1: Monitor logging");
  ptLogf("    board F_CPU=%lu Hz, millis=%lu", (unsigned long)F_CPU,
         (unsigned long)millis());
  ptLogf("    RGB constants resolved via %s",
         PT_RGB3_NAMED ? "named macros (LED3_R..LED4_B)"
                       : "LED_BUILTIN offsets (named macros absent)");
  recordResult("A1-log", PT_PASS, "Monitor output reached you");

  /* A2: matrix. The sequence is ordered so that each step narrows the fault:
   * if allOn works but a sweep does not, a scan line is bad; if the border is
   * missing, the outer rows/columns are not driven. */
  logStep("");
  logStep("A2: 8x13 LED matrix - WATCH THE PANEL");
  ptMatrixBegin();

  logStep("    [1/6] all pixels on (2s) - look for dead pixels");
  ptMatrixPatternAllOn();
  delay(2000);

  logStep("    [2/6] checkerboard, alternating (2s) - look for crosstalk");
  for (uint8_t i = 0; i < 4; ++i) {
    ptMatrixPatternCheckerboard(i & 0x01);
    delay(500);
  }

  logStep("    [3/6] border only (2s) - outer rows and columns must light");
  ptMatrixPatternBorder();
  delay(2000);

  logStep("    [4/6] row sweep - one horizontal row at a time");
  ptMatrixPatternRowSweep();

  logStep("    [5/6] column sweep - one vertical column at a time");
  ptMatrixPatternColSweep();

  logStep("    [6/6] scrolling text");
  ptMatrixScrollText("PHASE A MATRIX OK", 70);
  ptMatrixShowText2("OK");
  delay(1500);
  ptMatrixClear();

  recordResult("A2-matrix", PT_VISUAL,
               "Confirm all 6 patterns by eye; panel has no readback");

  /* A3: RGB LEDs. Each segment is lit alone first, because a shorted or
   * mis-mapped segment only shows up when the others are dark. White last
   * proves all three segments of one package can be on simultaneously. */
  logStep("");
  logStep("A3: onboard RGB LEDs - WATCH LED3 THEN LED4");
  rgbBegin();

  logStep("    LED3: red, green, blue, white, off (700ms each)");
  rgbSetLed3(true, false, false); delay(700);
  rgbSetLed3(false, true, false); delay(700);
  rgbSetLed3(false, false, true); delay(700);
  rgbSetLed3(true, true, true);   delay(700);
  rgbSetLed3(false, false, false);

  logStep("    LED4: red, green, blue, white, off (700ms each)");
  rgbSetLed4(true, false, false); delay(700);
  rgbSetLed4(false, true, false); delay(700);
  rgbSetLed4(false, false, true); delay(700);
  rgbSetLed4(true, true, true);   delay(700);
  rgbSetLed4(false, false, false);

  logStep("    Both: 3 alternating cycles");
  for (uint8_t i = 0; i < 3; ++i) {
    rgbSetLed3(true, false, false);
    rgbSetLed4(false, false, true);
    delay(400);
    rgbSetLed3(false, false, true);
    rgbSetLed4(true, false, false);
    delay(400);
  }
  rgbAllOff();

  recordResult("A3-rgb", PT_VISUAL,
               "Confirm colours and ACTIVE-LOW polarity by eye");

  logStep("");
  logStep("If a segment stayed DARK when commanded ON, or lit when commanded");
  logStep("OFF, the active-low assumption is wrong for this core release.");
  logStep("Report that back - it changes every RGB call in Stage 1b.");
}

#endif /* PHASE_A_BOARD_H */
