/*
 * PhishTap Lite — hw_selftest
 *
 * Operator prompting for tests that need a human to move a jumper.
 *
 * WHY THIS IS A SEPARATE MODULE
 *   R16: a test that needs a jumper moved must say so on the console AND on the
 *   matrix, and must wait for a card tap or a timed countdown. The console cannot
 *   take keystrokes, so there is no way to ask a question and read the answer
 *   back through it.
 *
 *   The matrix matters because it is the only output the operator can see without
 *   looking at the host. If the Serial monitor tab was opened late, or the cable
 *   is charge-only, the matrix is still there telling them what the board wants.
 *
 *   Both wait paths are supported. A card tap ends the wait immediately, which is
 *   the fast path once Phase B has proven the reader. A countdown always runs as
 *   the fallback, because the whole reason Phase L and Phase W exist is that the
 *   reader may not work, and a test that can only be advanced by a broken reader
 *   could never be completed.
 */

#ifndef PT_PROMPT_H
#define PT_PROMPT_H

#include <Arduino.h>

#include "pt_log.h"
#include "pt_matrix.h"

/* Default grace period for a wiring change. Long enough to unplug seven jumpers
 * and fit one, short enough that a mistaken wait does not feel like a hang. */
#ifndef PT_WIRING_WAIT_S
#define PT_WIRING_WAIT_S 25
#endif

/*
 * Optional early-exit hook.
 *
 * Phase L runs with the reader disconnected, so there is nothing to tap and this
 * is NULL. Phase W and Phase B run with it wired, so they pass a function that
 * polls for a card. Keeping it as a function pointer rather than calling the RFID
 * layer directly avoids a circular include: the prompt helper is needed by the
 * RFID phase, so it cannot depend on it.
 *
 * Returns true if the operator tapped, meaning "I am ready, continue now".
 */
typedef bool (*PtTapPollFn)(void);

/*
 * Announce a wiring change and wait for the operator.
 *
 * `matrixCode` is a one or two character status shown large on the panel during
 * the wait, per the T7 status-code convention. `instruction` is scrolled once so
 * the operator standing in front of the board knows what to do without reading
 * the host console.
 *
 * Returns true if the wait ended by card tap, false if it ran out the clock. The
 * distinction is logged but does not change the outcome of the test, since either
 * way the operator had the full opportunity to make the change.
 */
inline bool ptWaitForWiring(const char* matrixCode,
                            const char* instruction,
                            uint16_t waitSeconds,
                            PtTapPollFn tapPoll) {
  logBlank();
  logStep("----------------------------------------------------------");
  logStep("WIRING CHANGE REQUIRED");
  logStep(instruction);
  logStep("----------------------------------------------------------");

  /* Scroll the instruction on the panel. Short wording, because the matrix is 13
   * columns wide and anything longer takes a while to scroll past. */
  ptMatrixScrollText(matrixCode, 70);
  ptMatrixScrollText(instruction, 55);

  if (waitSeconds == 0) waitSeconds = PT_WIRING_WAIT_S;

  char digit[3];
  digit[2] = '\0';

  for (int s = (int)waitSeconds; s > 0; --s) {
    /* Two-digit countdown so the operator can see how much time is left rather
     * than just that something is happening. */
    digit[0] = (char)('0' + ((s / 10) % 10));
    digit[1] = (char)('0' + (s % 10));
    ptMatrixShowText2(digit);

    ptLogf("    waiting %2d s - tap the card to continue now, or let it run out",
           s);

    /* Poll in short slices rather than delay(1000) so a tap is noticed promptly
     * instead of up to a second late. */
    for (uint8_t i = 0; i < 10; ++i) {
      if (tapPoll != NULL && tapPoll()) {
        logStep("    Card tapped - continuing.");
        ptMatrixClear();
        return true;
      }
      delay(100);
    }
  }

  logStep("    Countdown finished - continuing.");
  ptMatrixClear();
  return false;
}

/* Convenience wrapper for the common case: no tap available, default wait. */
inline void ptWaitForWiringNoTap(const char* matrixCode,
                                 const char* instruction) {
  ptWaitForWiring(matrixCode, instruction, PT_WIRING_WAIT_S, NULL);
}

#endif /* PT_PROMPT_H */
