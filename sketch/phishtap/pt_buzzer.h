/*
 * PhishTap Lite — production firmware (Stage 1b)
 *
 * Non-blocking buzzer rhythm engine for the TMB12A05.
 *
 * WHY RHYTHM AND NOT PITCH
 * The TMB12A05 is an ACTIVE electromagnetic buzzer: it carries its own
 * oscillator and produces a fixed ~2.3 kHz tone whenever it has power. The
 * pitch cannot be varied from software, so every cue in the game has to be
 * distinguished by rhythm alone. That is a hardware fact recorded in
 * docs/HARDWARE.md, not a design preference.
 *
 * It is driven with digitalWrite only. No PWM and no tone(): there is no
 * frequency to generate, and this board's PWM is not a general-purpose audio
 * clock, so using it would add a failure mode for zero benefit.
 *
 * WHY NON-BLOCKING
 * hw_selftest/pt_buzzer.h plays these same rhythms with delay(), which is fine
 * there because a diagnostic owns the whole board while it runs. Production
 * cannot: the core's loop is `for(;;) { loop(); __loopHook(); }`, so any delay()
 * here would stall the RFID poll, the countdown and the bridge handlers all at
 * once. A 300 ms beep would mean 300 ms of not noticing a card. This engine is
 * therefore a millis()-driven state machine that returns immediately and
 * advances a little on each tick.
 *
 * Pattern names and timings match hw_selftest verbatim, so the rhythms that were
 * listened to and confirmed during bring-up are the same ones the game plays.
 */

#ifndef PT_BUZZER_H
#define PT_BUZZER_H

#include <Arduino.h>

#include "pt_pins.h"
#include "pt_config.h"

/* Wire vocabulary from docs/ARCHITECTURE.md §4. These names arrive as strings
 * through pt.buzz, so they are part of the protocol contract. */
typedef enum : uint8_t {
  BUZZ_WELCOME = 0,   /* card accepted, session created      */
  BUZZ_JOIN,          /* phone connected to the session      */
  BUZZ_OK,            /* correct answer                      */
  BUZZ_WRONG,         /* wrong answer or timeout             */
  BUZZ_TICK,          /* countdown tick                      */
  BUZZ_BUSY,          /* tapped while a session is active    */
  BUZZ_OVER,          /* game over                           */
  BUZZ_ERROR,         /* hardware fault                      */
  BUZZ_PATTERN_COUNT
} PtBuzzPattern;

inline const char* ptBuzzName(PtBuzzPattern p) {
  switch (p) {
    case BUZZ_WELCOME: return "welcome";
    case BUZZ_JOIN:    return "join";
    case BUZZ_OK:      return "ok";
    case BUZZ_WRONG:   return "wrong";
    case BUZZ_TICK:    return "tick";
    case BUZZ_BUSY:    return "busy";
    case BUZZ_OVER:    return "over";
    case BUZZ_ERROR:   return "error";
    default:           return "?";
  }
}

/* Parse a pattern name from pt.buzz. Returns false for anything unrecognised so
 * the handler can report an error instead of silently playing nothing, which
 * would look identical to a broken buzzer. */
inline bool ptBuzzFromName(const char* name, PtBuzzPattern* out) {
  if (name == NULL || out == NULL) return false;
  for (uint8_t i = 0; i < (uint8_t)BUZZ_PATTERN_COUNT; ++i) {
    if (strcmp(ptBuzzName((PtBuzzPattern)i), name) == 0) {
      *out = (PtBuzzPattern)i;
      return true;
    }
  }
  return false;
}

/* Every pattern is expressible as (pulse count, beep length, gap length), which
 * keeps all the timing data in one table instead of scattered across a switch
 * full of delay() calls. Verified against hw_selftest's descriptions. */
typedef struct {
  uint8_t  pulses;
  uint16_t onMs;
  uint16_t gapMs;
  uint8_t  priority;   /* higher wins; see ptBuzzPlay */
} PtBuzzSpec;

/* Priority exists because cues overlap in normal play. A countdown tick fires
 * every 60-500 ms, and a WRONG cue can start while one is sounding. Without a
 * priority the tick would truncate the wrong-answer buzz, and the player would
 * hear a cue cut short for no reason they can perceive.
 *
 * The rule is: a cue interrupts only cues of equal or lower priority. TICK is
 * the lowest, so it never interrupts anything, but everything interrupts it.
 * ERROR is the highest, because a hardware fault is the one thing that must be
 * heard over whatever else is happening. */
static const PtBuzzSpec s_buzzSpecs[BUZZ_PATTERN_COUNT] = {
  /*            pulses  onMs  gapMs  priority */
  /* WELCOME */ {  1,     80,    0,     3   },
  /* JOIN    */ {  2,     80,   80,     3   },
  /* OK      */ {  1,    120,    0,     3   },
  /* WRONG   */ {  3,     60,   60,     5   },
  /* TICK    */ {  1,     25,    0,     1   },
  /* BUSY    */ {  2,    250,  100,     4   },
  /* OVER    */ {  2,    300,  100,     6   },
  /* ERROR   */ {  5,     40,   40,     7   },
};

inline const char* ptBuzzDescription(PtBuzzPattern p) {
  switch (p) {
    case BUZZ_WELCOME: return "1 beep, 80ms";
    case BUZZ_JOIN:    return "2 beeps, 80ms each, 80ms gap";
    case BUZZ_OK:      return "1 beep, 120ms";
    case BUZZ_WRONG:   return "3 beeps, 60ms each, 60ms gap";
    case BUZZ_TICK:    return "1 short blip, 25ms";
    case BUZZ_BUSY:    return "2 long beeps, 250ms each, 100ms gap";
    case BUZZ_OVER:    return "300ms, 100ms gap, 300ms";
    case BUZZ_ERROR:   return "5 beeps, 40ms each, 40ms gap";
    default:           return "?";
  }
}

/* ── Engine state ───────────────────────────────────────────────────────── */

static struct {
  bool     active;
  bool     onPhase;      /* true while the buzzer is sounding */
  uint8_t  pulseIndex;   /* how many beeps have started */
  uint8_t  pulses;
  uint16_t onMs;
  uint16_t gapMs;
  uint8_t  priority;
  uint32_t phaseStart;
} s_buzz;

inline void ptBuzzBegin(void) {
  pinMode(PT_BUZZER_PIN, OUTPUT);
  /* Drive OFF explicitly. The 10 kR base-emitter pulldown already holds the
   * transistor off while this pin floats during reset, but writing the idle
   * level here means the first instruction after pinMode cannot beep. */
  digitalWrite(PT_BUZZER_PIN, PT_BUZZER_OFF);
  memset(&s_buzz, 0, sizeof(s_buzz));
}

inline void ptBuzzSilence(void) {
  digitalWrite(PT_BUZZER_PIN, PT_BUZZER_OFF);
  s_buzz.active   = false;
  s_buzz.onPhase  = false;
  s_buzz.priority = 0;
}

inline bool ptBuzzBusy(void) {
  return s_buzz.active;
}

/* Start a pattern, interrupting whatever is playing only if the new cue is at
 * least as important. Returns true if the pattern was accepted.
 *
 * Returning false is not an error condition the caller needs to escalate; it
 * means "a more important cue is already sounding", which is the intended
 * behaviour. The countdown engine relies on this to fire ticks without ever
 * having to check what else is playing.
 */
inline bool ptBuzzPlay(PtBuzzPattern p) {
  if (p >= BUZZ_PATTERN_COUNT) return false;
  const PtBuzzSpec* spec = &s_buzzSpecs[p];

  if (s_buzz.active && spec->priority < s_buzz.priority) return false;

  s_buzz.active     = true;
  s_buzz.onPhase    = true;
  s_buzz.pulseIndex = 0;
  s_buzz.pulses     = spec->pulses;
  s_buzz.onMs       = spec->onMs;
  s_buzz.gapMs      = spec->gapMs;
  s_buzz.priority   = spec->priority;
  s_buzz.phaseStart = millis();

  digitalWrite(PT_BUZZER_PIN, PT_BUZZER_ON);
  return true;
}

/* Advance the current pattern. Called once per loop() iteration; does nothing
 * unless a phase boundary has been crossed, so the cost when idle is a single
 * boolean test.
 *
 * Timing uses `now - phaseStart >= duration` rather than comparing absolute
 * deadlines. That form is immune to millis() wrapping, which happens after about
 * 49.7 days and would otherwise produce one very long or very short beep on a
 * console left running for weeks. */
inline void ptBuzzTick(void) {
  if (!s_buzz.active) return;

  uint32_t now = millis();
  uint16_t dur = s_buzz.onPhase ? s_buzz.onMs : s_buzz.gapMs;
  if ((uint32_t)(now - s_buzz.phaseStart) < dur) return;

  s_buzz.phaseStart = now;

  if (s_buzz.onPhase) {
    /* The beep just ended. */
    digitalWrite(PT_BUZZER_PIN, PT_BUZZER_OFF);
    s_buzz.pulseIndex++;
    if (s_buzz.pulseIndex >= s_buzz.pulses) {
      s_buzz.active  = false;
      s_buzz.onPhase = false;
      return;
    }
    /* A gap follows, unless the spec has none, in which case the pattern is
     * already complete because every remaining pulse would need a gap first. */
    if (s_buzz.gapMs == 0) {
      s_buzz.active  = false;
      s_buzz.onPhase = false;
      return;
    }
    s_buzz.onPhase = false;
  } else {
    /* The gap just ended, so start the next beep. */
    s_buzz.onPhase = true;
    digitalWrite(PT_BUZZER_PIN, PT_BUZZER_ON);
  }
}

#endif /* PT_BUZZER_H */
