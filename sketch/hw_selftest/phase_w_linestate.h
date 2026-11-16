/*
 * PhishTap Lite — hw_selftest
 *
 * PHASE W — MISO line-state probe. RC522 WIRED.
 *
 * WHAT THIS PROVES
 *   Phase B's 0x00 at every clock says MISO is low for the whole transfer but not
 *   why. This phase stops clocking entirely and just looks at the electrical state
 *   of the line, which separates the remaining suspects without a multimeter.
 *
 *   The trick is chip select. With SS held HIGH the RC522 is deselected and its
 *   MISO output must be high impedance, so whatever the STM32's internal pull
 *   resistor says is what we read. With SS held LOW the chip is selected and
 *   drives the line. Comparing the two tells us whether the module is connected
 *   and powered at all.
 *
 *   The decisive case is an UNPOWERED module. Its MISO pin then clamps low through
 *   the input ESD diode, so the line reads 0 even with the internal pull-up
 *   enabled. That is a different reading from a genuinely floating line, which
 *   follows the pull in both directions, and it is the single most likely cause
 *   given the JANALOG header crowding.
 *
 * R17: INPUT_PULLDOWN is not assumed. It was verified present in this core
 *   (cores/arduino/api/Common.h defines INPUT_PULLDOWN = 0x3, and
 *   wiring_digital.cpp maps it to GPIO_PULL_DOWN), but the code still guards it
 *   with #ifdef and reports which path was taken, so a future core release that
 *   drops it degrades to pull-up-only instead of failing to build.
 *
 * MATRIX CODES: shows "W" while running, "OK" or "ER" at the end.
 */

#ifndef PHASE_W_LINESTATE_H
#define PHASE_W_LINESTATE_H

#include <Arduino.h>
#include <SPI.h>

#include "pt_pins.h"
#include "pt_log.h"
#include "pt_matrix.h"
#include "pt_prompt.h"
#include "pt_spi_probe.h"

/* Report whether the core gave us a real pull-down. R17 requires this to be
 * stated rather than assumed. */
#ifdef INPUT_PULLDOWN
  #define PT_HAS_PULLDOWN 1
#else
  #define PT_HAS_PULLDOWN 0
#endif

/*
 * Read D12 (MISO) under a given pin mode, several times.
 *
 * Read repeatedly rather than once because a floating pin picks up whatever is
 * nearby and can drift. A line that is genuinely floating often gives a mix of
 * readings, while a line that is driven gives the same answer every time. That
 * difference is diagnostic, so the spread is reported and not averaged away.
 */
static void ptWProbeMiso(PinMode mode, const char* modeName, bool ssHigh,
                         uint8_t* outOnes, uint8_t* outSamples) {
  const uint8_t samples = 8;
  uint8_t ones = 0;

  /* Drive SS to the required level BEFORE switching D12 to an input, so the chip
   * is already in the intended select state when we look at the line. */
  digitalWrite(PT_RC522_SS, ssHigh ? HIGH : LOW);
  delay(2);

  pinMode(PT_RC522_MISO, mode);
  delay(2);

  for (uint8_t i = 0; i < samples; ++i) {
    if (digitalRead(PT_RC522_MISO) == HIGH) ones++;
    delay(1);
  }

  *outOnes = ones;
  *outSamples = samples;
  ptLogf("    SS=%s  D12 as %-14s -> %u/%u reads HIGH",
         ssHigh ? "HIGH" : "LOW ", modeName, (unsigned)ones, (unsigned)samples);
}

/* Classify a probe: all high, all low, or mixed. Mixed means floating and noisy
 * rather than driven. */
static const char* ptWClassify(uint8_t ones, uint8_t samples) {
  if (ones == samples) return "always HIGH";
  if (ones == 0)       return "always LOW";
  return "MIXED (floating/noisy)";
}

inline void phaseW_run(void) {
  logSection("PHASE W - MISO LINE STATE (RC522 WIRED)");

  ptMatrixShowText2("W");

  /* R16: announce the wiring requirement on the console and the matrix, and wait.
   * A tap poll is offered here because the reader is wired, but the countdown
   * always runs as well: this phase exists precisely because the reader may not
   * work, so it must not depend on it. */
  ptWaitForWiring(
      "W",
      "RC522 WIRED: 3V3 GND RST=D5 SS=D10 SCK=D13 MOSI=D11 MISO=D12. NO LOOPBACK JUMPER.",
      PT_WIRING_WAIT_S,
      NULL);

  logStep("");
  ptLogf("W0: INPUT_PULLDOWN support on this core: %s",
         PT_HAS_PULLDOWN ? "AVAILABLE" : "NOT AVAILABLE (pull-up only)");
  if (!PT_HAS_PULLDOWN) {
    logStep("    Falling back to pull-up probing only. The unpowered-module case");
    logStep("    is still detectable, but a driven-high line cannot be confirmed.");
  }

  /* The bus is brought up so D10, D11 and D13 are in their SPI roles, then D10 is
   * taken back as a plain output so chip select can be held at a known level
   * while D12 is examined. The overlay declares st,gpio-control, which is what
   * makes that legal. */
  ptSpiBusBegin();
  pinMode(PT_RC522_SS, OUTPUT);

  logStep("");
  logStep("W1: probing D12 (MISO) with no clock running");
  logStep("    With SS HIGH the RC522 is deselected and must release MISO, so the");
  logStep("    reading is set by the STM32's own pull resistor.");
  logStep("    With SS LOW the chip is selected and should drive the line.");
  logStep("");

  uint8_t ones, samples;
  uint8_t hiPuOnes, hiPuS, hiPdOnes, hiPdS, hiNpOnes, hiNpS;
  uint8_t loPuOnes, loPuS, loPdOnes, loPdS;

  /* --- SS HIGH: expect high impedance, so the line follows the pull --- */
  ptWProbeMiso(INPUT,          "INPUT (no pull)", true, &hiNpOnes, &hiNpS);
  ptWProbeMiso(INPUT_PULLUP,   "INPUT_PULLUP",    true, &hiPuOnes, &hiPuS);
#if PT_HAS_PULLDOWN
  ptWProbeMiso(INPUT_PULLDOWN, "INPUT_PULLDOWN",  true, &hiPdOnes, &hiPdS);
#else
  hiPdOnes = 0; hiPdS = 0;
  logStep("    SS=HIGH  D12 as INPUT_PULLDOWN -> skipped, not supported");
#endif

  /* --- SS LOW: chip selected, so it should drive MISO --- */
  ptWProbeMiso(INPUT_PULLUP,   "INPUT_PULLUP",    false, &loPuOnes, &loPuS);
#if PT_HAS_PULLDOWN
  ptWProbeMiso(INPUT_PULLDOWN, "INPUT_PULLDOWN",  false, &loPdOnes, &loPdS);
#else
  loPdOnes = 0; loPdS = 0;
  logStep("    SS=LOW   D12 as INPUT_PULLDOWN -> skipped, not supported");
#endif

  /* Leave SS deselected before interpreting, so the module is not left selected
   * with a pin still configured as an input. */
  digitalWrite(PT_RC522_SS, HIGH);

  logStep("");
  logStep("W2: interpretation");
  logStep("    SS HIGH, pull-up   : " );
  ptLogf("      %s", ptWClassify(hiPuOnes, hiPuS));
#if PT_HAS_PULLDOWN
  logStep("    SS HIGH, pull-down : ");
  ptLogf("      %s", ptWClassify(hiPdOnes, hiPdS));
#endif
  logStep("    SS LOW,  pull-up   : ");
  ptLogf("      %s", ptWClassify(loPuOnes, loPuS));

  /* ── Decision ─────────────────────────────────────────────────────────────
   * The combinations below are the ones that actually distinguish the suspects.
   * Each branch states the conclusion AND the next action, because a diagnosis
   * the operator cannot act on is only half useful.
   */
  bool hiFollowsPull = (hiPuOnes == hiPuS);
#if PT_HAS_PULLDOWN
  hiFollowsPull = hiFollowsPull && (hiPdOnes == 0);
#endif
  bool hiStuckLow  = (hiPuOnes == 0);
  bool hiStuckHigh = (hiPuOnes == hiPuS) && (hiNpOnes == hiNpS);
#if PT_HAS_PULLDOWN
  hiStuckHigh = hiStuckHigh && (hiPdOnes == hiPdS);
#endif
  bool loDrivesLow = (loPuOnes == 0);

  logStep("");

  if (hiStuckLow) {
    ptMatrixShowText2("ER");
    recordResult("W-miso", PT_FAIL,
                 "MISO held LOW even with pull-up, SS deselected");
    logStep("W3: DECISION - MISO is being held low while the chip is deselected.");
    logStep("    A deselected SPI slave must release the line, so the internal");
    logStep("    pull-up should have won. It did not. Three explanations:");
    logStep("");
    logStep("    a) THE MODULE IS NOT POWERED. This is the most likely. An");
    logStep("       unpowered RC522 clamps MISO low through its input ESD diode,");
    logStep("       which looks exactly like a short to ground from out here.");
    logStep("       -> Do the LED power test in the README against the 3V3 and");
    logStep("          GND header pins to prove which physical pins they are.");
    logStep("       -> JANALOG order is BOOT, IOREF, RESET, +3V3, +5V, GND, GND,");
    logStep("          VIN. The 3V3 pin sits between RESET and +5V, so being one");
    logStep("          pin off either reboots the MCU or puts 5V into a 2.5-3.3V");
    logStep("          part. Check the silkscreen, not the position.");
    logStep("");
    logStep("    b) MISO IS SHORTED TO GND, or the jumper is in a GND hole.");
    logStep("");
    logStep("    c) THE MODULE IS DAMAGED, for example by 5V on VCC.");
  } else if (hiFollowsPull && loDrivesLow) {
    ptMatrixShowText2("OK");
    recordResult("W-miso", PT_PASS,
                 "Hi-Z when deselected, driven when selected");
    logStep("W3: DECISION - the line behaves correctly.");
    logStep("    Deselected it follows the pull, so the module releases MISO as");
    logStep("    it should. Selected it pulls the line low, so something on the");
    logStep("    other end is actively driving it.");
    logStep("");
    logStep("    => The RC522 is POWERED, CONNECTED and responding to chip");
    logStep("       select. The wiring and the power are fine.");
    logStep("");
    logStep("    That moves the suspicion onto the protocol layer rather than the");
    logStep("    electrical one. Re-run Phase B and look hard at B0: if the raw");
    logStep("    register fingerprint still reads 0x00 while this phase says the");
    logStep("    chip drives MISO, suspect the address byte framing, the SPI");
    logStep("    mode, or that SCK is not actually reaching the module.");
  } else if (hiFollowsPull) {
    ptMatrixShowText2("ER");
    recordResult("W-miso", PT_FAIL,
                 "MISO floating in BOTH select states - wire not connected");
    logStep("W3: DECISION - MISO follows the internal pull whether the chip is");
    logStep("    selected or not. Nothing is ever driving it.");
    logStep("");
    logStep("    => The MISO wire is not making contact, or is on the wrong pin.");
    logStep("       Check, in order:");
    logStep("       1. The MISO jumper is actually in D12 (PB14) and seated.");
    logStep("       2. MISO and MOSI are not swapped. This is the most common");
    logStep("          wiring error and module silkscreen order varies by seller.");
    logStep("       3. The module's header pins are soldered. Cheap RC522 boards");
    logStep("          often ship with unsoldered headers that look fitted.");
    logStep("       4. The module is powered. An unpowered module can also leave");
    logStep("          MISO undriven if its ESD clamp is not conducting.");
  } else if (hiStuckHigh) {
    ptMatrixShowText2("ER");
    recordResult("W-miso", PT_FAIL, "MISO held HIGH in both select states");
    logStep("W3: DECISION - MISO is held high even when deselected and even with");
    logStep("    a pull-down fitted. Something is driving it high or it is shorted");
    logStep("    to the 3V3 rail.");
    logStep("    -> Check for a solder bridge or a jumper landing on 3V3/IOREF.");
    logStep("    -> Note that IOREF is a 3V3 OUTPUT and sits two pins from 3V3 on");
    logStep("       JANALOG, so a misplaced jumper there would do exactly this.");
  } else {
    ptMatrixShowText2("ER");
    recordResult("W-miso", PT_FAIL, "MISO readings inconsistent, see table");
    logStep("W3: DECISION - the readings do not fit a clean pattern. Mixed or");
    logStep("    drifting values usually mean a floating line picking up noise,");
    logStep("    which points at a bad contact somewhere on the MISO path.");
    logStep("    -> Re-seat every jumper, keep them under 20cm, and re-run.");
    logStep("    -> Paste the full W1 table back; the exact mix matters.");
  }

  /* Restore D12 to its SPI role. It was reconfigured as a plain input above, and
   * leaving it that way would break every later phase. Calling SPI.begin() again
   * re-applies the pinmux for the whole bus. */
  logStep("");
  logStep("W4: restoring D11/D12/D13 to their SPI function");
  ptSpiBusBegin();
  pinMode(PT_RC522_MISO, INPUT);
  ptSpiBusBegin();
  logStep("    SPI.begin() re-run. Later phases can use the bus normally.");

  logStep("");
  logStep("Phase W complete.");
}

#endif /* PHASE_W_LINESTATE_H */
