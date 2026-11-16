/*
 * PhishTap Lite — hw_selftest
 *
 * Bring-up diagnostic for the Arduino UNO Q. Runs the smoke-test phases in the
 * order the hardware is wired, and reports a verdict for each.
 *
 *   Phase A   board only, 0 wires      matrix + onboard RGB LEDs + logging
 *   Phase B   7 wires                  MFRC522 / RC522 over SPI
 *   Phase C1  +3 wires                 one LED on D7
 *   Phase C2  +6 wires                 three lives LEDs on D7/D8/D9
 *   Phase C3  +5 wires and a diode     buzzer on D6 via an NPN switch
 *
 * Phase B runs before Phase C deliberately. The RC522 is the only component
 * whose failure would force a redesign, so it is worth knowing about that before
 * spending time on LEDs and a buzzer.
 *
 * HOW TO RUN
 *   Wire only what the phase you are running needs. Set the RUN_PHASE_* flags
 *   below to 1 for the phases you have wired and 0 for the rest, then flash.
 *   Leaving them all at 1 runs the whole sequence, which is what you want once
 *   the board is fully assembled.
 *
 * WHAT "VISUAL" MEANS
 *   The matrix, the LEDs and the buzzer have no readback path, so the firmware
 *   cannot observe whether they actually worked. Those rows are reported as
 *   VISUAL and you must confirm them by eye or by ear. Only Phase B can produce
 *   a genuine machine-verified PASS or FAIL.
 *
 * Read the README.md in this folder for flashing instructions.
 */

#include <Arduino.h>

/* ── Phase selection ────────────────────────────────────────────────────────
 * These MUST be defined before including pt_runner.h. That header expands
 * `#if RUN_PHASE_A` and friends at include time, and an undefined macro in an
 * #if evaluates to 0 — so defining them afterwards would silently compile out
 * every phase instead of producing an error.
 *
 * Overridable from the build command line so the same source can be compiled for
 * a partially wired board without editing it. tools/deploy_to_board.sh rewrites
 * these defaults for you via --phases.
 */
#ifndef RUN_PHASE_A
#define RUN_PHASE_A 1
#endif
#ifndef RUN_PHASE_L
#define RUN_PHASE_L 0
#endif
#ifndef RUN_PHASE_W
#define RUN_PHASE_W 0
#endif
#ifndef RUN_PHASE_B
#define RUN_PHASE_B 1
#endif
#ifndef RUN_PHASE_C1
#define RUN_PHASE_C1 1
#endif
#ifndef RUN_PHASE_C2
#define RUN_PHASE_C2 1
#endif
#ifndef RUN_PHASE_C3
#define RUN_PHASE_C3 1
#endif

/* Repeat the whole selftest forever instead of idling after one pass. Useful
 * when the Serial monitor was opened too late to catch the first run. */
#ifndef PT_LOOP_FOREVER
#define PT_LOOP_FOREVER 0
#endif

/* pt_runner.h pulls in every module and must be included last, because it calls
 * the phase entry points. The individual headers are not listed here so there is
 * exactly one place that defines the include order. */
#include "pt_runner.h"

#define PT_SELFTEST_VERSION "1.2.0"

void setup() {
  /*
   * The console comes up first because every later phase reports through it.
   *
   * On this board Serial and Monitor are the SAME object: the UNO Q variant
   * defines ARDUINO_ROUTERBRIDGE_PROVIDES_SERIAL, and Arduino_RouterBridge's
   * singletons.cpp declares
   *     extern BridgeMonitor<> Serial [[gnu::alias("Monitor")]];
   * so writing to either reaches App Lab's "Serial monitor" tab. The baud rate
   * shown in that tab is cosmetic: BridgeMonitor::begin() explicitly discards
   * both of its arguments.
   *
   * If logInit() returns false the router link is not established, which means
   * the arduino-router service is not running. There is no useful fallback:
   * the STM32U585 has no USB of its own, so Monitor is the only output channel.
   * The phases still run, so the visual and audible checks remain usable.
   */
  bool consoleOk = logInit();

  logBlank();
  Monitor.println("##############################################");
  Monitor.println("# PhishTap Lite - HARDWARE SELFTEST          #");
  Monitor.println("##############################################");
  ptLogf("version %s", PT_SELFTEST_VERSION);
  ptLogf("phases enabled: A=%d L=%d W=%d B=%d C1=%d C2=%d C3=%d",
         RUN_PHASE_A, RUN_PHASE_L, RUN_PHASE_W, RUN_PHASE_B,
         RUN_PHASE_C1, RUN_PHASE_C2, RUN_PHASE_C3);
  logStep("Read this in App Lab's Serial monitor tab (called 'Sketch' in some");
  logStep("App Lab versions). Not the Python or App Launch tab.");
  if (!consoleOk) {
    logStep("WARNING: Monitor.begin() reported the router link is not ready.");
    logStep("         Output may be lost. Check: systemctl status arduino-router");
  }

  ptMatrixBegin();
  ptMatrixScrollText("SELFTEST", 70);

  /* Give the operator time to open the Serial monitor before anything important
   * is printed. Without this the whole run can finish before the tab is open,
   * which is indistinguishable from "the sketch produced no output". */
  ptStartupCountdown();

  ptRunAllPhases();

  logBlank();
  logStep("SELFTEST COMPLETE. Copy everything above back verbatim.");
  logStep("If the reader is working, tapping a card now replays the buzzer");
  logStep("patterns so you can listen again without reflashing.");

  ptMatrixScrollText("DONE", 70);
  ptMatrixShowText2("OK");

  /* Leave the outputs in a safe, quiet state rather than whatever the last
   * pattern happened to end on. */
  livesAllOff();
  buzzerOff();
  rgbAllOff();
}

void loop() {
#if PT_LOOP_FOREVER
  /* Re-run everything. The countdown runs again too, so each pass is visibly
   * delimited on the matrix and the console keeps producing fresh output even
   * if the monitor was connected late. */
  delay(3000);
  resetResults();
  logBlank();
  Monitor.println("########## RE-RUNNING SELFTEST ##########");
  ptStartupCountdown();
  ptRunAllPhases();
  livesAllOff();
  buzzerOff();
  rgbAllOff();
  return;
#endif

  /*
   * Idle, with one convenience: replay the buzzer vocabulary on a card tap.
   *
   * WHY this is worth the code: the audible check is the one result a human can
   * get wrong by being distracted, and re-running it should not require a
   * reflash. It reuses the reader that Phase B already proved, so it adds no new
   * hardware dependency.
   *
   * The 200 ms sleep keeps this from spinning the CPU. It is a blocking call,
   * which is fine in a finished diagnostic and would not be acceptable in the
   * production sketch's loop().
   */
  delay(200);

  if (!s_rfidUsable) return;

  if (s_rfid.PICC_IsNewCardPresent()) {
    if (s_rfid.PICC_ReadCardSerial()) {
      logBlank();
      logStep("Card tapped - replaying all buzzer patterns.");
      static const PtBuzzPattern order[] = {
        BUZZ_WELCOME, BUZZ_JOIN, BUZZ_OK, BUZZ_WRONG,
        BUZZ_TICK,    BUZZ_BUSY, BUZZ_OVER, BUZZ_ERROR
      };
      for (uint8_t i = 0; i < sizeof(order) / sizeof(order[0]); ++i) {
        ptLogf("    '%s': %s", buzzPatternName(order[i]),
               buzzPatternDescription(order[i]));
        buzzPlayPattern(order[i]);
        delay(400);
      }
      logStep("Replay done.");
      ptMatrixShowText2("OK");
    }
    s_rfid.PICC_HaltA();
    s_rfid.PCD_StopCrypto1();

    /* Wait for the card to leave before arming again, otherwise one tap reads as
     * many and the patterns replay back to back. */
    rfidWaitRemoval(3000UL);
  }
}
