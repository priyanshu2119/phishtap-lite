/*
 * PhishTap Lite — hw_selftest
 *
 * Phase runner and startup countdown.
 *
 * WHY THIS FILE EXISTS
 *   The selftest emits everything from setup(), within the first seconds after
 *   the sketch starts. App Lab's Serial monitor tab shows the MCU console, but if
 *   that tab is not already open and connected the early output is simply missed
 *   — which looks exactly like "the sketch printed nothing".
 *
 *   A visible countdown on the LED matrix before any phase runs gives the
 *   operator time to open the monitor. The matrix is the only output channel
 *   guaranteed to be visible without any host tooling, which is why the countdown
 *   goes there rather than to the console.
 *
 *   This lives in a header, not in the .ino, because arduino-cli hoists ctags
 *   prototypes for every function defined in the .ino and those prototypes cannot
 *   see types declared later in the file. See the ".ino purity rule" in
 *   tools/syntax_check.sh. The .ino may define only setup() and loop().
 *
 * INCLUDE ORDER: this header must come AFTER every phase_*.h, because it calls
 * their entry points.
 */

#ifndef PT_RUNNER_H
#define PT_RUNNER_H

#include <Arduino.h>

#include "pt_pins.h"
#include "pt_log.h"
#include "pt_matrix.h"
#include "pt_prompt.h"
#include "pt_buzzer.h"
#include "pt_spi_probe.h"
#include "phase_a_board.h"
#include "phase_l_loopback.h"
#include "phase_w_linestate.h"
#include "phase_b_rfid.h"
#include "phase_c_outputs.h"

/* Seconds of grace before the first phase runs. Long enough to switch to the
 * Serial monitor tab and see it connect, short enough not to be annoying on a
 * repeat run. Overridable from the build command line. */
#ifndef PT_STARTUP_COUNTDOWN_S
#define PT_STARTUP_COUNTDOWN_S 8
#endif

/*
 * Show a countdown on the matrix.
 *
 * Each second is announced with a single digit and one short buzzer blip, so the
 * operator gets both a visual and an audible cue that the sketch is alive and
 * waiting. The buzzer is only sounded if Phase C3 is compiled in, since that is
 * the phase that proves the buzzer is wired; beeping before then could be
 * mistaken for a fault.
 */
inline void ptStartupCountdown(void) {
  char digit[2];
  digit[1] = '\0';

  ptLogf("Waiting %d s before starting so you can open the Serial monitor tab.",
         (int)PT_STARTUP_COUNTDOWN_S);
  logStep("Open App Lab's SERIAL MONITOR tab now - that is the MCU console.");

  for (int s = PT_STARTUP_COUNTDOWN_S; s > 0; --s) {
    digit[0] = (char)('0' + (s % 10));
    ptMatrixShowText2(digit);
    ptLogf("  starting in %d ...", s);
    delay(1000);
  }
  ptMatrixClear();
}

/*
 * Run every enabled phase once, then print the summary.
 *
 * The order is deliberate: least wiring first, and the diagnostics that isolate a
 * fault before the test that depends on everything working. Phase L needs the
 * reader REMOVED and a single MOSI-to-MISO jumper, Phase W needs it wired again,
 * so running both in one pass means moving wires twice. Each phase announces what
 * it needs on the console and the matrix and waits, per R16.
 *
 * The RUN_PHASE_* macros are compile-time, so a disabled phase costs nothing but
 * a SKIP row in the summary. Keeping the sequence in one function lets loop()
 * repeat the whole run without duplicating the dispatch, which matters because
 * duplication here is exactly how the two copies of this sketch drifted apart
 * once before.
 */
inline void ptRunAllPhases(void) {
  /* Phase L and Phase B have CONTRADICTORY wiring requirements: L needs the RC522
   * completely removed with a D11-D12 loopback jumper fitted, while B needs the
   * reader wired with that jumper absent. Enabling both in one run guarantees one
   * of them is executed with the wrong wiring, and the resulting failure looks
   * like a hardware fault rather than a test-setup mistake.
   *
   * This happened for real: a combined run reported L-loopback FAIL because the
   * reader was still attached and driving MISO, which buried the fact that B
   * went on to read a card perfectly. Warn up front rather than let the operator
   * decode a misleading summary. */
#if RUN_PHASE_L && (RUN_PHASE_B || RUN_PHASE_W)
  logBlank();
  logStep("!!! WARNING: Phase L is enabled together with Phase W/B. !!!");
  logStep("    L needs the RC522 REMOVED and a D11-D12 jumper fitted.");
  logStep("    W and B need the RC522 WIRED and that jumper removed.");
  logStep("    One of them WILL run with the wrong wiring and report a failure");
  logStep("    that is not a hardware fault. Run them separately:");
  logStep("      tools/run_phase.sh L      then      tools/run_phase.sh diag");
  logBlank();
  ptMatrixScrollText("L AND B CONFLICT", 60);
  delay(2000);
#endif

#if RUN_PHASE_A
  phaseA_run();
#else
  recordResult("A", PT_SKIP, "Disabled at compile time");
#endif

#if RUN_PHASE_L
  phaseL_run();
#else
  recordResult("L", PT_SKIP, "Disabled at compile time");
#endif

#if RUN_PHASE_W
  phaseW_run();
#else
  recordResult("W", PT_SKIP, "Disabled at compile time");
#endif

#if RUN_PHASE_B
  phaseB_run();
#else
  recordResult("B", PT_SKIP, "Disabled at compile time");
#endif

#if RUN_PHASE_C1
  phaseC1_run();
#else
  recordResult("C1", PT_SKIP, "Disabled at compile time");
#endif

#if RUN_PHASE_C2
  phaseC2_run();
#else
  recordResult("C2", PT_SKIP, "Disabled at compile time");
#endif

#if RUN_PHASE_C3
  phaseC3_run();
#else
  recordResult("C3", PT_SKIP, "Disabled at compile time");
#endif

  printSummary();
}

#endif /* PT_RUNNER_H */
