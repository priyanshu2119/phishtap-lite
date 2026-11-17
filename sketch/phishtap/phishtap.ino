/*
 * PhishTap Lite — production firmware (Stage 1b)
 *
 * Arduino UNO Q, MCU side. Drives the RC522 reader, the 8x13 LED matrix, three
 * lives LEDs, the onboard RGB status LEDs and the buzzer, and exposes the game
 * console to Python over the internal bridge.
 *
 * ── THE .ino PURITY RULE ──────────────────────────────────────────────────
 * This file defines ONLY setup() and loop(). Nothing else. Not a helper, not a
 * constant, not a type.
 *
 * arduino-cli runs ctags over the .ino and INSERTS A PROTOTYPE for every
 * function it finds there, above the rest of the file. A hoisted prototype for a
 * function returning a type declared in a later header produces
 * "'PtState' was not declared in this scope", and one for a function that already
 * has a definition produces "redeclared as different kind of entity". Both stop
 * the build, and neither is diagnosable from the .ino, because the offending
 * code is not visible in it. hw_selftest hit exactly this and the fix was to
 * abandon the merged single-file layout entirely. tools/syntax_check.sh enforces
 * the rule so it cannot regress.
 *
 * ── INCLUDE CHAIN ─────────────────────────────────────────────────────────
 * pt_bridge.h is the top of the chain and pulls in everything else:
 *
 *   pt_bridge.h  -> pt_app.h -> pt_config.h, pt_pins.h, pt_json.h, pt_state.h,
 *                              pt_events.h, pt_font.h, pt_matrix.h, pt_buzzer.h,
 *                              pt_lives.h, pt_rgb.h, pt_countdown.h, pt_rfid.h
 *
 * Order matters and is not arbitrary: pt_json.h and pt_events.h are pure C++ with
 * no Arduino dependency so they can be unit-tested on a laptop; pt_state.h and
 * pt_config.h define the vocabulary the engines use; the engines come next; and
 * pt_app.h ties them together before pt_bridge.h exposes them over RPC.
 *
 * ── THREADING ─────────────────────────────────────────────────────────────
 * Single-threaded, and provably so. The core's main is
 * `for(;;) { loop(); __loopHook(); }`, and __loopHook is what dispatches every
 * provide_safe handler. So handlers run on this thread, strictly between
 * iterations of loop(), never during one. That is why there is no mutex, no
 * volatile and no atomic anywhere in this firmware.
 */

#include "pt_bridge.h"

void setup() {
  /* Hardware first, bridge second. Registering handlers before ptAppBegin()
   * would let Python observe a zeroed ptApp and conclude the console was idle
   * and healthy when in fact nothing had been initialised yet. */
  ptAppBegin();

  bool bridgeOk = ptBridgeBegin();
  if (!bridgeOk) {
    /* Keep running rather than halting. The console is still usable as a
     * standalone attract-mode box, the fault is queued so Python sees it the
     * moment the link does come up, and the ERROR state makes it visible on the
     * panel. A firmware that stops in setup() gives nobody anything to debug. */
    ptRingPushHwFault(&ptRing, millis(), PT_FAULT_BRIDGE);
    ptAppSetState(PT_ST_ERROR);
    Serial.println("bridge: registration FAILED, continuing in ERROR state");
  }
}

void loop() {
  /* Advance every engine. Non-blocking throughout: this returns in well under a
   * millisecond when nothing is due, which is what keeps the bridge handlers
   * responsive, since they only run once loop() has returned. */
  ptAppLoop();

  /* Emit any log line a handler deferred. Handlers cannot print, because
   * Monitor traffic rides the same router as the RPC itself and printing from
   * inside a response deadlocks it. */
  ptBridgePollLogs();
}
