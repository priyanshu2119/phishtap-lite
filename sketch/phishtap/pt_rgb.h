/*
 * PhishTap Lite — production firmware (Stage 1b)
 *
 * Onboard RGB status LEDs. LED3 shows the console state as a colour; LED4 pulses
 * while the console is waiting for a phone to join.
 *
 * POLARITY: ACTIVE-LOW. Writing LOW lights a segment, writing HIGH darkens it.
 * This is the OPPOSITE of the external lives LEDs in pt_lives.h, and is verified
 * rather than assumed: the official arduino/app-bricks-examples "unoq-pin-toggle"
 * sketch writes HIGH to all six segments in setup() specifically to turn them
 * OFF, and its Python side marks each one {"active_low": True}.
 *
 * PT_RGB_ON/PT_RGB_OFF exist so no call site ever writes a bare HIGH or LOW,
 * which is how an inverted-polarity bug survives code review.
 *
 * LED1 and LED2 belong to Linux and are never touched here.
 *
 * ONLY EIGHT COLOURS, AND WHY THAT IS FINE
 * These segments are driven as plain digital outputs, so each is either on or off
 * and there are exactly eight reachable colours. That is enough for a state
 * indicator whose job is "glanceable, distinct, unambiguous". Fading would need
 * PWM on these pins, which the UNO Q does not expose for them, and would add a
 * dependency on a timer for no functional gain.
 */

#ifndef PT_RGB_H
#define PT_RGB_H

#include <Arduino.h>

#include "pt_pins.h"
#include "pt_state.h"

/* Which of the two RGB LEDs a call refers to. */
#define PT_RGB_LED3 3
#define PT_RGB_LED4 4

/* Half-period of the LED4 "waiting for a phone" pulse. 450 ms is slow enough to
 * read as a calm invitation rather than an alarm, and fast enough to notice in
 * peripheral vision. */
#define PT_RGB_PULSE_HALF_MS 450

inline void ptRgbSetSegment(uint8_t pin, bool on) {
  /* Active-low: ON means drive the pin LOW. */
  digitalWrite(pin, on ? PT_RGB_ON : PT_RGB_OFF);
}

/* Set one LED to an RGB combination. Each component is a boolean because the
 * segments are digital, not PWM. */
inline void ptRgbSet(uint8_t led, bool r, bool g, bool b) {
  if (led == PT_RGB_LED3) {
    ptRgbSetSegment(PT_RGB3_R, r);
    ptRgbSetSegment(PT_RGB3_G, g);
    ptRgbSetSegment(PT_RGB3_B, b);
  } else if (led == PT_RGB_LED4) {
    ptRgbSetSegment(PT_RGB4_R, r);
    ptRgbSetSegment(PT_RGB4_G, g);
    ptRgbSetSegment(PT_RGB4_B, b);
  }
}

static PtState  s_rgbState;
static bool     s_rgbPulseOn;
static uint32_t s_rgbPulseStart;

inline void ptRgbBegin(void) {
  /* All six segments must be initialised to OFF before anything else, because
   * the pins float at reset and a floating active-low input can light a segment
   * faintly. This mirrors what the official example sketch does. */
  pinMode(PT_RGB3_R, OUTPUT);
  pinMode(PT_RGB3_G, OUTPUT);
  pinMode(PT_RGB3_B, OUTPUT);
  pinMode(PT_RGB4_R, OUTPUT);
  pinMode(PT_RGB4_G, OUTPUT);
  pinMode(PT_RGB4_B, OUTPUT);
  ptRgbSet(PT_RGB_LED3, false, false, false);
  ptRgbSet(PT_RGB_LED4, false, false, false);
  s_rgbState      = PT_ST_BOOT;
  s_rgbPulseOn    = false;
  s_rgbPulseStart = millis();
}

/* The colour for each state. Chosen so that no two states a player can move
 * between directly share a colour, and so that red is reserved exclusively for
 * ERROR — if red ever means something benign, it stops meaning anything.
 *
 *   BOOT      blue     starting up, not ready
 *   SELFTEST  white    exercising every output
 *   IDLE      green    ready, come and play
 *   TAPPED    blue     card read, waiting for a phone
 *   PLAYING   cyan     a round is live
 *   OVER      magenta  game finished
 *   BUSY      yellow   occupied, tap refused
 *   ERROR     red      hardware fault
 */
inline void ptRgbApplyState(PtState s) {
  switch (s) {
    case PT_ST_BOOT:     ptRgbSet(PT_RGB_LED3, false, false, true);  break;
    case PT_ST_SELFTEST: ptRgbSet(PT_RGB_LED3, true,  true,  true);  break;
    case PT_ST_IDLE:     ptRgbSet(PT_RGB_LED3, false, true,  false); break;
    case PT_ST_TAPPED:   ptRgbSet(PT_RGB_LED3, false, false, true);  break;
    case PT_ST_PLAYING:  ptRgbSet(PT_RGB_LED3, false, true,  true);  break;
    case PT_ST_OVER:     ptRgbSet(PT_RGB_LED3, true,  false, true);  break;
    case PT_ST_BUSY:     ptRgbSet(PT_RGB_LED3, true,  true,  false); break;
    case PT_ST_ERROR:    ptRgbSet(PT_RGB_LED3, true,  false, false); break;
    default:             ptRgbSet(PT_RGB_LED3, false, false, false); break;
  }
}

/* Called when the state changes. Kept separate from the tick so a state change
 * takes effect immediately rather than waiting up to one pulse half-period, which
 * would make the console feel laggy exactly when the player is looking at it. */
inline void ptRgbSetState(PtState s) {
  if (s == s_rgbState) return;
  s_rgbState     = s;
  s_rgbPulseOn   = false;
  s_rgbPulseStart = millis();
  ptRgbApplyState(s);

  /* LED4 pulses only while a phone is awaited, and ptRgbTick drives that. In
   * every other state it is dark, because a pulsing LED that means nothing
   * trains people to ignore it. */
  ptRgbSet(PT_RGB_LED4, false, false, false);
}

inline PtState ptRgbGetState(void) {
  return s_rgbState;
}

inline void ptRgbTick(void) {
  if (s_rgbState != PT_ST_TAPPED) {
    /* Not awaiting a phone, so LED4 stays off and there is nothing to time. */
    if (s_rgbPulseOn) {
      s_rgbPulseOn = false;
      ptRgbSet(PT_RGB_LED4, false, false, false);
    }
    return;
  }

  uint32_t now = millis();
  if ((uint32_t)(now - s_rgbPulseStart) < PT_RGB_PULSE_HALF_MS) return;
  s_rgbPulseStart = now;
  s_rgbPulseOn = !s_rgbPulseOn;

  /* Blue pulse, matching LED3's TAPPED colour so the two LEDs read as one
   * message rather than two unrelated lights. */
  ptRgbSet(PT_RGB_LED4, false, false, s_rgbPulseOn);
}

/* Both LEDs dark. Used when the link to Python is lost, so the console does not
 * keep advertising a state it can no longer guarantee. */
inline void ptRgbAllOff(void) {
  ptRgbSet(PT_RGB_LED3, false, false, false);
  ptRgbSet(PT_RGB_LED4, false, false, false);
  s_rgbPulseOn = false;
}

#endif /* PT_RGB_H */
