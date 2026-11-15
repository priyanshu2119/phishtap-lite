/*
 * PhishTap Lite — hw_selftest
 *
 * PHASE C — external outputs. C1 one LED, C2 three lives LEDs, C3 the buzzer.
 *
 * These are the only checks in the selftest that can be wrong because of a
 * calculation rather than a library, so each one states the arithmetic it depends
 * on and asks the operator to confirm the physical result.
 */

#ifndef PHASE_C_OUTPUTS_H
#define PHASE_C_OUTPUTS_H

#include <Arduino.h>

#include "pt_pins.h"
#include "pt_log.h"
#include "pt_buzzer.h"
#include "phase_b_rfid.h"   /* for ptConfirm() and the reader state */

/* ── Lives LEDs ─────────────────────────────────────────────────────────────
 * Wiring is pin -> 150 R -> LED anode (long leg) -> LED cathode -> GND, so the
 * pin SOURCES current and the LED is ACTIVE-HIGH. That is the opposite of the
 * onboard RGB LEDs tested in Phase A. Getting this backwards produces LEDs that
 * are on when they should be off, which still looks like "working" at a glance
 * and is therefore worth stating explicitly.
 *
 * Resistor maths for a typical red LED at Vf = 2.0 V:
 *     R = (3.3 - 2.0) / 0.009 = 144 R  ->  150 R (E12)  ->  I = 8.7 mA
 * That sits comfortably under the ~20 mA per-pin limit and leaves headroom for
 * the port-total budget when all three are lit at once (3 x 8.7 = 26 mA).
 *
 * A different colour needs a different resistor, because Vf changes:
 *     red/amber  Vf ~2.0 V -> 150 R gives ~8.7 mA
 *     green      Vf ~2.1 V -> 150 R gives ~8.0 mA
 *     blue/white Vf ~3.0 V -> 150 R gives ~2.0 mA (dim); use 68 R for ~4.4 mA
 * Never go below the value that keeps the pin at 10 mA or less.
 */
/* Forward declaration: livesBegin() calls livesSetCount(), defined below.
 * Header-only code gets no Arduino auto-prototypes, so the order matters. */
inline void livesSetCount(uint8_t n);

inline void livesBegin(void) {
  pinMode(PT_LIFE_LED_1, OUTPUT);
  pinMode(PT_LIFE_LED_2, OUTPUT);
  pinMode(PT_LIFE_LED_3, OUTPUT);
  livesSetCount(0);
}

/* Light the first n LEDs, as the game will during play. */
inline void livesSetCount(uint8_t n) {
  digitalWrite(PT_LIFE_LED_1, (n >= 1) ? PT_LIFE_ON : PT_LIFE_OFF);
  digitalWrite(PT_LIFE_LED_2, (n >= 2) ? PT_LIFE_ON : PT_LIFE_OFF);
  digitalWrite(PT_LIFE_LED_3, (n >= 3) ? PT_LIFE_ON : PT_LIFE_OFF);
}

inline void livesAllOff(void) { livesSetCount(0); }

/* ── C1: a single LED on D7 ─────────────────────────────────────────────────
 * The smallest possible external-output test. If this fails, nothing else in
 * Phase C can work, and the fault is either the resistor, the LED polarity, or
 * the pin.
 */
inline void phaseC1_run(void) {
  logSection("PHASE C1 - ONE LED ON D7 (3 wires)");
  logStep("Wiring: D7 -> 150R -> LED anode (long leg) -> LED cathode -> GND");
  logStep("");

  livesBegin();

  logStep("    Blinking D7 five times, 400ms on / 400ms off");
  for (uint8_t i = 0; i < 5; ++i) {
    digitalWrite(PT_LIFE_LED_1, PT_LIFE_ON);
    delay(400);
    digitalWrite(PT_LIFE_LED_1, PT_LIFE_OFF);
    delay(400);
  }

  unsigned char c = ptConfirm("C1: did the LED on D7 blink five times?", 30000UL);
  recordResult("C1-led", verdictFromConfirm(c),
               c == PT_CONFIRM_NA ? "D7 blink - judge by eye"
                                  : (c == PT_CONFIRM_YES ? "D7 blinked 5x"
                                                         : "D7 did not blink"));

  if (c == PT_CONFIRM_NO) {
    logStep("    CHECK:");
    logStep("      1. LED polarity - the LONG leg is the anode and goes to the");
    logStep("         resistor side. Reversed, it simply will not light.");
    logStep("      2. The resistor is actually 150R (brown-green-brown), not a");
    logStep("         much larger value that would make it invisibly dim.");
    logStep("      3. A blue or white LED at 150R is very dim. Try 68R.");
    logStep("      4. The jumper is in D7, not D8 or D9.");
  }
  livesAllOff();
}

/* ── C2: all three lives LEDs ───────────────────────────────────────────────
 * Proves D7, D8 and D9 all work and that the game's "count down from 3" display
 * will read correctly. The chase is followed by the actual game representation,
 * 3 -> 2 -> 1 -> 0, because that is what a player will see and it is the version
 * worth confirming.
 */
inline void phaseC2_run(void) {
  logSection("PHASE C2 - THREE LIVES LEDs (6 wires)");
  logStep("Wiring: D7/D8/D9 each -> 150R -> LED anode -> cathode -> GND");
  logStep("");

  livesBegin();

  logStep("    [1/2] chase: D7 -> D8 -> D9, three passes");
  for (uint8_t pass = 0; pass < 3; ++pass) {
    for (uint8_t which = 0; which < 3; ++which) {
      digitalWrite(PT_LIFE_LED_1, which == 0 ? PT_LIFE_ON : PT_LIFE_OFF);
      digitalWrite(PT_LIFE_LED_2, which == 1 ? PT_LIFE_ON : PT_LIFE_OFF);
      digitalWrite(PT_LIFE_LED_3, which == 2 ? PT_LIFE_ON : PT_LIFE_OFF);
      delay(250);
    }
  }

  logStep("    [2/2] game view: 3 lives, then 2, then 1, then 0");
  for (uint8_t n = 3; ; --n) {
    livesSetCount(n);
    ptLogf("      lives = %u", (unsigned)n);
    delay(900);
    if (n == 0) break;
  }
  delay(600);
  livesSetCount(3);
  delay(1200);

  unsigned char c = ptConfirm("C2: did all THREE LEDs chase, then count 3-2-1-0?",
                              30000UL);
  recordResult("C2-lives", verdictFromConfirm(c),
               c == PT_CONFIRM_NA ? "Chase + 3-2-1-0 - judge by eye"
                                  : (c == PT_CONFIRM_YES ? "All three LEDs correct"
                                                         : "One or more LEDs wrong"));

  if (c == PT_CONFIRM_NO) {
    logStep("    CHECK: note WHICH position failed. A single dark LED is a");
    logStep("    wiring or polarity fault on that pin; two or more dark LEDs");
    logStep("    suggests a shared GND that is not actually connected.");
  }
  livesAllOff();
}

/* ── C3: the buzzer ─────────────────────────────────────────────────────────
 * The TMB12A05 is an active buzzer with a fixed internal oscillator, so there is
 * exactly one pitch and every cue is a rhythm. This phase plays the full pattern
 * vocabulary that the game will use, announcing each one first so the operator
 * can check the rhythm and not merely "a sound happened".
 *
 * Code passing here proves nothing about sound. The transistor may be the wrong
 * way round, the flyback diode may be reversed, the buzzer may be backwards, or
 * the 5 V rail may be absent - all of those produce a silent but perfectly
 * successful digitalWrite. This is the one phase that MUST be confirmed by ear.
 */
inline void phaseC3_run(void) {
  logSection("PHASE C3 - BUZZER (5 wires + diode)");
  logStep("Wiring: D6 -> 1k -> NPN base; 10k base -> GND; emitter -> GND;");
  logStep("        collector -> buzzer '-'; buzzer '+' -> 5V pin;");
  logStep("        1N4007 across the buzzer, CATHODE (stripe) to '+'.");
  logStep("");
  logStep("BUZZER TEST: you should now hear each pattern as it is named.");
  logStep("");

  buzzerBegin();

  static const PtBuzzPattern order[] = {
    BUZZ_WELCOME, BUZZ_JOIN, BUZZ_OK, BUZZ_WRONG,
    BUZZ_TICK,    BUZZ_BUSY, BUZZ_OVER, BUZZ_ERROR
  };
  const uint8_t orderCount = (uint8_t)(sizeof(order) / sizeof(order[0]));

  /* Two passes. The first lets the operator hear each pattern once in sequence;
   * the second repeats them so a missed or mis-heard pattern can be caught
   * without restarting the sketch. */
  for (uint8_t pass = 0; pass < 2; ++pass) {
    ptLogf("    --- pass %u of 2 ---", (unsigned)(pass + 1));
    for (uint8_t i = 0; i < orderCount; ++i) {
      PtBuzzPattern p = order[i];
      ptLogf("    now playing '%s': %s", buzzPatternName(p),
             buzzPatternDescription(p));
      buzzPlayPattern(p);
      delay(500);
    }
  }

  logStep("");
  logStep("    Now playing the countdown tick at an accelerating rate,");
  logStep("    which is how it behaves during a real round.");
  uint16_t gap = 500;
  for (uint8_t i = 0; i < 12; ++i) {
    buzzPlayPattern(BUZZ_TICK);
    delay(gap);
    /* Shrink the gap but never below 60ms: below that the 25ms blips start to
     * merge into a continuous tone and stop reading as a countdown. */
    gap = (gap > 100) ? (uint16_t)(gap - 40) : 60;
  }

  unsigned char c = ptConfirm("C3: did you HEAR all the patterns above?", 45000UL);
  recordResult("C3-buzzer", verdictFromConfirm(c),
               c == PT_CONFIRM_NA ? "All patterns - judge BY EAR"
                                  : (c == PT_CONFIRM_YES ? "Audible, rhythms correct"
                                                         : "Silent or wrong rhythm"));

  if (c == PT_CONFIRM_NO) {
    logStep("    CHECK, in this order:");
    logStep("      1. TRANSISTOR PINOUT. TO-92 flats differ by part AND maker:");
    logStep("         PN2222A is E-B-C, but P2N2222A and BC547 are C-B-E.");
    logStep("         Read the marking on the body, look up THAT marking, and");
    logStep("         confirm the base with a multimeter diode test.");
    logStep("         A hot transistor means it is backwards - power off now.");
    logStep("      2. Buzzer polarity: the case is marked '+'. That side goes to");
    logStep("         the 5V rail, the other to the collector.");
    logStep("      3. 1N4007 orientation: the silver stripe (cathode) to '+'.");
    logStep("      4. Is the 5V pin actually 5V? Measure it. If the board is");
    logStep("         USB-powered from a weak port the rail may be sagging.");
    logStep("      5. The TMB12A05 needs at least 4.0V across it. Measure the");
    logStep("         voltage ACROSS THE BUZZER while it should be sounding.");
    logStep("      6. Sanity-check the buzzer itself: touch it directly across");
    logStep("         5V and GND with the correct polarity. A steady tone means");
    logStep("         it is active and working; a single click means it is passive");
    logStep("         or dead.");
  }
  buzzerOff();
}

#endif /* PHASE_C_OUTPUTS_H */
