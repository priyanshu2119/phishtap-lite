/*
 * PhishTap Lite — hw_selftest
 *
 * Buzzer rhythm engine for the TMB12A05 active buzzer.
 */

#ifndef PT_BUZZER_H
#define PT_BUZZER_H

#include <Arduino.h>

#include "pt_pins.h"

/*
 * The TMB12A05 is an ACTIVE electromagnetic buzzer: it carries its own
 * oscillator and produces its fixed ~2.3 kHz tone whenever it has power. Two
 * consequences drive this whole module.
 *
 * 1. It is driven with digitalWrite only. There is no frequency to generate, so
 *    no PWM and no tone(). That is fortunate rather than merely convenient —
 *    this board's PWM is not a general-purpose audio clock, and using it here
 *    would add a failure mode for zero benefit.
 *
 * 2. The pitch CANNOT be varied. Every cue in the game must therefore be
 *    distinguished by rhythm alone, which is why the patterns below differ in
 *    pulse count, pulse length and gap length rather than in tone. The pattern
 *    names match the Bridge RPC vocabulary from the architecture document so the
 *    production sketch can reuse them verbatim.
 *
 * WHY blocking delays are acceptable here: this is a one-shot diagnostic that
 * owns the whole board while it runs. The production sketch replaces this with a
 * millis()-driven pattern state machine so loop() never stalls; doing that in the
 * selftest would add moving parts to the thing whose job is to isolate faults.
 */

typedef enum {
  BUZZ_WELCOME = 0,   /* card accepted, session created      */
  BUZZ_JOIN,          /* phone connected to the session        */
  BUZZ_OK,            /* correct answer                        */
  BUZZ_WRONG,         /* wrong answer or timeout               */
  BUZZ_TICK,          /* countdown tick; rate rises over time  */
  BUZZ_BUSY,          /* tapped while a session is active      */
  BUZZ_OVER,          /* game over                             */
  BUZZ_ERROR          /* hardware fault                        */
} PtBuzzPattern;

inline const char* buzzPatternName(PtBuzzPattern p) {
  switch (p) {
    case BUZZ_WELCOME: return "welcome";
    case BUZZ_JOIN:    return "join";
    case BUZZ_OK:      return "ok";
    case BUZZ_WRONG:   return "wrong";
    case BUZZ_TICK:    return "tick";
    case BUZZ_BUSY:    return "busy";
    case BUZZ_OVER:    return "over";
    case BUZZ_ERROR:   return "error";
  }
  return "?";
}

/* Human-readable description of the rhythm, printed before each pattern so the
 * operator knows what they are supposed to be listening for. Without this the
 * audible test is meaningless: you cannot tell "wrong rhythm" from "no sound at
 * all" if you were never told what correct sounds like. */
inline const char* buzzPatternDescription(PtBuzzPattern p) {
  switch (p) {
    case BUZZ_WELCOME: return "1 beep, 80ms";
    case BUZZ_JOIN:    return "2 beeps, 80ms each, 80ms gap";
    case BUZZ_OK:      return "1 beep, 120ms";
    case BUZZ_WRONG:   return "3 beeps, 60ms each, 60ms gap";
    case BUZZ_TICK:    return "1 short blip, 25ms";
    case BUZZ_BUSY:    return "2 long beeps, 250ms each, 100ms gap";
    case BUZZ_OVER:    return "300ms, 100ms gap, 300ms";
    case BUZZ_ERROR:   return "5 beeps, 40ms each, 40ms gap";
  }
  return "?";
}

inline void buzzerBegin(void) {
  pinMode(PT_BUZZER_PIN, OUTPUT);
  /* Drive OFF explicitly. The 10 kR base-emitter pulldown already holds the
   * transistor off while this pin floats during reset, but writing the idle
   * level here means the very first instruction after pinMode cannot beep. */
  digitalWrite(PT_BUZZER_PIN, PT_BUZZER_OFF);
}

inline void buzzerOn(void)  { digitalWrite(PT_BUZZER_PIN, PT_BUZZER_ON);  }
inline void buzzerOff(void) { digitalWrite(PT_BUZZER_PIN, PT_BUZZER_OFF); }

inline void buzzerBeep(uint16_t onMs) {
  buzzerOn();
  delay(onMs);
  buzzerOff();
}

/* Play one pattern. `pulses` is the beep count, `onMs` the beep length and
 * `gapMs` the silence between beeps; every pattern in the table is expressible
 * in those three numbers, which keeps the timing data in one place. */
inline void buzzPlayPattern(PtBuzzPattern p) {
  switch (p) {
    case BUZZ_WELCOME:
      buzzerBeep(80);
      break;
    case BUZZ_JOIN:
      buzzerBeep(80); delay(80); buzzerBeep(80);
      break;
    case BUZZ_OK:
      buzzerBeep(120);
      break;
    case BUZZ_WRONG:
      for (uint8_t i = 0; i < 3; ++i) {
        buzzerBeep(60);
        if (i < 2) delay(60);
      }
      break;
    case BUZZ_TICK:
      buzzerBeep(25);
      break;
    case BUZZ_BUSY:
      for (uint8_t i = 0; i < 2; ++i) {
        buzzerBeep(250);
        if (i < 1) delay(100);
      }
      break;
    case BUZZ_OVER:
      buzzerBeep(300); delay(100); buzzerBeep(300);
      break;
    case BUZZ_ERROR:
      for (uint8_t i = 0; i < 5; ++i) {
        buzzerBeep(40);
        if (i < 4) delay(40);
      }
      break;
  }
}

#endif /* PT_BUZZER_H */
