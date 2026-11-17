/*
 * PhishTap Lite — production firmware (Stage 1b)
 *
 * Lives LED engine: three external LEDs on D7/D8/D9, plus the non-blocking blink
 * used when a life is lost.
 *
 * POLARITY, AND WHY IT IS STATED TWICE
 * These are wired pin -> 150 R -> LED anode -> cathode -> GND, so the pin
 * SOURCES current and the LEDs are ACTIVE-HIGH. The onboard RGB LEDs in
 * pt_rgb.h are ACTIVE-LOW. Opposite polarity, two modules, one project — this is
 * the single most likely wiring and coding bug in the whole build, which is why
 * PT_LIFE_ON/PT_LIFE_OFF exist as names rather than anyone writing HIGH or LOW
 * at a call site.
 *
 * NON-BLOCKING
 * Losing a life blinks the remaining LEDs. With delay() that blink would freeze
 * the RFID poll and the countdown, so it is a millis() state machine driven from
 * loop() like every other engine here.
 */

#ifndef PT_LIVES_H
#define PT_LIVES_H

#include <Arduino.h>

#include "pt_pins.h"
#include "pt_config.h"

static const uint8_t s_lifePins[PT_MAX_LIVES] = {
  PT_LIFE_LED_1, PT_LIFE_LED_2, PT_LIFE_LED_3
};

static struct {
  uint8_t  count;        /* how many lives are currently lit */
  bool     blinking;
  uint8_t  blinkLeft;    /* blinks remaining */
  bool     blinkOn;      /* current half-cycle is lit */
  uint32_t blinkStart;
} s_lives;

/* Blink timing. 120 ms on and 120 ms off is fast enough to read as a single
 * urgent event and slow enough to actually see; three cycles is about 720 ms,
 * which fits comfortably inside the pause before the next round starts. */
#define PT_LIFE_BLINK_HALF_MS  120
#define PT_LIFE_BLINK_CYCLES   3

inline void ptLivesBegin(void) {
  for (uint8_t i = 0; i < PT_MAX_LIVES; ++i) {
    pinMode(s_lifePins[i], OUTPUT);
    digitalWrite(s_lifePins[i], PT_LIFE_OFF);
  }
  memset(&s_lives, 0, sizeof(s_lives));
}

/* Light exactly `n` LEDs, from the first pin upwards. Values above PT_MAX_LIVES
 * are clamped rather than rejected: the display is cosmetic, and refusing to
 * update would leave a stale and more misleading count on the panel. The clamp
 * also protects the pin array from an out-of-bounds index if Python ever sends
 * something corrupt. */
inline void ptLivesSetCount(uint8_t n) {
  if (n > PT_MAX_LIVES) n = PT_MAX_LIVES;
  s_lives.count    = n;
  s_lives.blinking = false;
  for (uint8_t i = 0; i < PT_MAX_LIVES; ++i) {
    digitalWrite(s_lifePins[i], (i < n) ? PT_LIFE_ON : PT_LIFE_OFF);
  }
}

inline uint8_t ptLivesGetCount(void) {
  return s_lives.count;
}

inline void ptLivesAllOff(void) {
  ptLivesSetCount(0);
}

/* Start the lost-a-life blink. The count is dropped first so the blink shows the
 * NEW number of lives, which is what the player needs to see; blinking the old
 * count and then changing it would read as two unrelated events. */
inline void ptLivesLoseOne(void) {
  uint8_t next = (s_lives.count > 0) ? (uint8_t)(s_lives.count - 1) : 0;
  s_lives.count      = next;
  s_lives.blinking   = true;
  s_lives.blinkLeft  = PT_LIFE_BLINK_CYCLES * 2;   /* on and off halves */
  s_lives.blinkOn    = true;
  s_lives.blinkStart = millis();
}

inline bool ptLivesBlinking(void) {
  return s_lives.blinking;
}

inline void ptLivesTick(void) {
  if (!s_lives.blinking) return;

  uint32_t now = millis();
  if ((uint32_t)(now - s_lives.blinkStart) < PT_LIFE_BLINK_HALF_MS) return;
  s_lives.blinkStart = now;

  /* Drive the pins for this half-cycle. During the "on" halves the surviving
   * LEDs light; during the "off" halves everything is dark. */
  for (uint8_t i = 0; i < PT_MAX_LIVES; ++i) {
    bool lit = s_lives.blinkOn && (i < s_lives.count);
    digitalWrite(s_lifePins[i], lit ? PT_LIFE_ON : PT_LIFE_OFF);
  }

  s_lives.blinkOn = !s_lives.blinkOn;
  if (s_lives.blinkLeft > 0) s_lives.blinkLeft--;

  if (s_lives.blinkLeft == 0) {
    /* Settle on the steady state so the panel is never left dark by accident
     * when the blink ends on an "off" half-cycle. */
    s_lives.blinking = false;
    for (uint8_t i = 0; i < PT_MAX_LIVES; ++i) {
      digitalWrite(s_lifePins[i], (i < s_lives.count) ? PT_LIFE_ON : PT_LIFE_OFF);
    }
  }
}

#endif /* PT_LIVES_H */
