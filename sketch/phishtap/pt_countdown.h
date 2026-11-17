/*
 * PhishTap Lite — production firmware (Stage 1b)
 *
 * The per-round countdown: an accelerating tick and a single timeout event.
 *
 * THE MCU IS NOT THE AUTHORITY ON TIMING
 * docs/ARCHITECTURE.md §5 is explicit that the server is the single source of
 * truth. The phone renders its countdown from a deadline the server supplies,
 * and if the MCU and the server disagree, the server wins. This engine exists
 * for two things the server cannot do: drive the buzzer tick locally so it stays
 * in time even if the bridge hiccups, and notice a timeout so the console can
 * react immediately rather than waiting for a round trip.
 *
 * That is why a timeout here queues an EVENT instead of changing game state.
 * Python receives round_timeout and decides what it means; the MCU does not
 * unilaterally end a round.
 *
 * ACCELERATING TICK
 * Cadence is clamp(remaining / 8, 60 ms, 500 ms), per §5. Dividing by eight
 * means eight ticks across whatever time is left, so the rhythm speeds up as the
 * round runs down without any per-round tuning. The bounds matter: unbounded, a
 * 14 s round would tick every 1.75 s (too sparse to feel like pressure) and the
 * last second would tick every 125 ms (fine), while a 4 s round would tick every
 * 500 ms at the start (too sparse). Clamping keeps both ends usable.
 */

#ifndef PT_COUNTDOWN_H
#define PT_COUNTDOWN_H

#include <Arduino.h>

#include "pt_config.h"

/* Bitmask returned by ptCountdownTick so a single call can report both a tick
 * and a timeout, which happen on the same iteration when time runs out exactly
 * as a tick falls due. */
#define PT_CD_NONE    0u
#define PT_CD_TICK    1u
#define PT_CD_TIMEOUT 2u

static struct {
  bool     active;
  uint16_t round;
  uint32_t limitMs;
  uint32_t startMs;
  uint32_t lastTickMs;
  bool     timedOut;
} s_cd;

inline void ptCountdownBegin(void) {
  memset(&s_cd, 0, sizeof(s_cd));
}

/* Arm the countdown for a round.
 *
 * Returns false if limitMs is outside the legal range. Rejecting rather than
 * clamping is deliberate: the architecture fixes the round length formula as
 * max(4, 14 - 0.8 * round_index) seconds, so a limit outside 4000..14000 ms
 * means Python computed something wrong. Clamping would hide that bug behind a
 * console that plays fine but scores oddly, which is much harder to trace than a
 * rejected call logged at the point it happened.
 */
inline bool ptCountdownStart(uint16_t round, uint32_t limitMs, uint32_t now) {
  if (limitMs < PT_ROUND_LIMIT_MIN_MS || limitMs > PT_ROUND_LIMIT_MAX_MS) {
    return false;
  }
  s_cd.active     = true;
  s_cd.round      = round;
  s_cd.limitMs    = limitMs;
  s_cd.startMs    = now;
  s_cd.lastTickMs = now;
  s_cd.timedOut   = false;
  return true;
}

inline void ptCountdownStop(void) {
  s_cd.active   = false;
  s_cd.timedOut = false;
}

inline bool ptCountdownActive(void) {
  return s_cd.active;
}

inline uint16_t ptCountdownRound(void) {
  return s_cd.round;
}

/* Milliseconds remaining, or 0 if inactive or expired.
 *
 * The subtraction is done as (start + limit) - now rather than
 * limit - (now - start) so that a millis() wraparound between start and now
 * still yields the right answer, provided the round itself is shorter than the
 * wrap period. At a 14 s maximum against a 49.7 day wrap, that always holds.
 */
inline uint32_t ptCountdownRemainingMs(uint32_t now) {
  if (!s_cd.active) return 0;
  uint32_t deadline = s_cd.startMs + s_cd.limitMs;
  if ((int32_t)(deadline - now) <= 0) return 0;
  return deadline - now;
}

/* The current tick cadence for the time remaining. Exposed separately from the
 * tick function so the value can be logged during bring-up; watching the cadence
 * shrink is the quickest way to confirm the acceleration is actually happening.
 */
inline uint32_t ptCountdownCadenceMs(uint32_t now) {
  uint32_t remaining = ptCountdownRemainingMs(now);
  uint32_t cadence   = remaining / 8;
  if (cadence < PT_COUNTDOWN_TICK_MIN_MS) cadence = PT_COUNTDOWN_TICK_MIN_MS;
  if (cadence > PT_COUNTDOWN_TICK_MAX_MS) cadence = PT_COUNTDOWN_TICK_MAX_MS;
  return cadence;
}

/* Advance the countdown. Returns a bitmask of PT_CD_TICK and PT_CD_TIMEOUT.
 *
 * Called once per loop() iteration with the same `now` every other engine got,
 * so all of them agree about what time it is.
 *
 * On timeout the countdown deactivates itself. Without that it would fire
 * round_timeout on every subsequent iteration, filling the event ring with
 * duplicates and pushing out the events that actually matter.
 */
inline uint8_t ptCountdownTick(uint32_t now) {
  if (!s_cd.active) return PT_CD_NONE;

  uint8_t out = PT_CD_NONE;

  uint32_t remaining = ptCountdownRemainingMs(now);
  if (remaining == 0) {
    /* Time is up. Report it once, then go quiet. */
    if (!s_cd.timedOut) {
      s_cd.timedOut = true;
      s_cd.active   = false;
      out |= PT_CD_TIMEOUT;
    }
    return out;
  }

  uint32_t cadence = ptCountdownCadenceMs(now);
  if ((uint32_t)(now - s_cd.lastTickMs) >= cadence) {
    s_cd.lastTickMs = now;
    out |= PT_CD_TICK;
  }
  return out;
}

#endif /* PT_COUNTDOWN_H */
