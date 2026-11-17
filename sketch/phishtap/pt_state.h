/*
 * PhishTap Lite — production firmware (Stage 1b)
 *
 * MCU state machine: the states, their wire names, and which of them Python is
 * allowed to set.
 *
 * WHY THE STATE IS AN ENUM AND NOT A STRING
 * The bridge carries state as a name, but comparing strings on every loop
 * iteration to decide what to animate is both slow and typo-prone. The enum is
 * the internal truth and the name is only its serialisation, so a misspelled
 * name from Python is rejected at the boundary instead of silently matching
 * nothing forever.
 *
 * DEVIATION FROM docs/ARCHITECTURE.md §4, DELIBERATE
 * The architecture lists the wire states as idle, tapped, playing, over, busy
 * and error. This firmware adds two more, "boot" and "selftest", covering the
 * BOOT -> SELFTEST -> IDLE startup path that §5 of the same document already
 * specifies for the MCU.
 *
 * The reason is that collapsing startup into "idle" loses information at the
 * exact moment it matters most. Python detects a dead MCU by the absence of
 * heartbeats, and at boot there is a window where the MCU is alive but not yet
 * ready. Reporting "idle" there is indistinguishable from reporting "ready and
 * waiting for a tap", so a boot that hangs inside the selftest would look like
 * a healthy idle console. Reporting the real state makes that failure obvious.
 *
 * Python must therefore treat an unrecognised state as "not ready" rather than
 * crashing, which is good practice regardless. ARCHITECTURE.md is updated to
 * match in the same change.
 */

#ifndef PT_STATE_H
#define PT_STATE_H

#include <Arduino.h>

typedef enum : uint8_t {
  PT_ST_BOOT = 0,     /* MCU has reset, nothing initialised yet            */
  PT_ST_SELFTEST,     /* boot selftest running, outputs being confirmed    */
  PT_ST_IDLE,         /* ready, attract mode, waiting for a card           */
  PT_ST_TAPPED,       /* card read, event queued, awaiting Python's verdict */
  PT_ST_PLAYING,      /* a round is live, countdown running                */
  PT_ST_OVER,         /* game finished, score being shown                  */
  PT_ST_BUSY,         /* a session is already active, tap refused          */
  PT_ST_ERROR,        /* a hardware fault was reported, recovery attempted */
  PT_ST_COUNT         /* sentinel, never a real state                      */
} PtState;

/* Wire names. These strings are part of the protocol contract: Python matches
 * on them, so renaming one is a breaking change and must bump
 * PT_PROTO_VERSION. */
inline const char* ptStateName(PtState s) {
  switch (s) {
    case PT_ST_BOOT:     return "boot";
    case PT_ST_SELFTEST: return "selftest";
    case PT_ST_IDLE:     return "idle";
    case PT_ST_TAPPED:   return "tapped";
    case PT_ST_PLAYING:  return "playing";
    case PT_ST_OVER:     return "over";
    case PT_ST_BUSY:     return "busy";
    case PT_ST_ERROR:    return "error";
    default:             return "unknown";
  }
}

/* Parse a state name from pt.set_state.
 *
 * Returns false for anything unrecognised, including NULL and the empty string.
 * The caller replies with an error rather than defaulting to some state,
 * because guessing here would let a typo in Python silently put the console
 * into attract mode mid-game.
 */
inline bool ptStateFromName(const char* name, PtState* out) {
  if (name == NULL || out == NULL) return false;
  for (uint8_t i = 0; i < (uint8_t)PT_ST_COUNT; ++i) {
    if (strcmp(ptStateName((PtState)i), name) == 0) {
      *out = (PtState)i;
      return true;
    }
  }
  return false;
}

/* Which states Python may set through pt.set_state.
 *
 * BOOT and SELFTEST are excluded on purpose. They describe the MCU's own
 * startup and Python has no business putting the console back into them; there
 * is no way to re-run the boot selftest short of a reset, and pretending
 * otherwise would leave the firmware in a state its own startup code never
 * produces. Everything else is Python's to command, because per
 * docs/ARCHITECTURE.md §5 the server is the authority on game flow.
 */
inline bool ptStateIsSettable(PtState s) {
  switch (s) {
    case PT_ST_IDLE:
    case PT_ST_TAPPED:
    case PT_ST_PLAYING:
    case PT_ST_OVER:
    case PT_ST_BUSY:
    case PT_ST_ERROR:
      return true;
    default:
      return false;
  }
}

/* Whether the console is in a state where a new card tap should be accepted.
 *
 * Only IDLE accepts a tap. In TAPPED a session is already being negotiated, in
 * PLAYING a game is live, in OVER the score is being shown, and in BUSY the
 * refusal is the whole point. ERROR refuses because the hardware cannot be
 * trusted to have read the card correctly.
 *
 * BOOT and SELFTEST refuse because the reader may not be initialised yet, and
 * polling an uninitialised MFRC522 object is how a boot loop starts.
 */
inline bool ptStateAcceptsTap(PtState s) {
  return s == PT_ST_IDLE;
}

/* Whether the round countdown should be running. Kept as a function rather than
 * an inline comparison so the rule has one home; if a future state also needs a
 * countdown, this is the only place to change. */
inline bool ptStateRunsCountdown(PtState s) {
  return s == PT_ST_PLAYING;
}

#endif /* PT_STATE_H */
