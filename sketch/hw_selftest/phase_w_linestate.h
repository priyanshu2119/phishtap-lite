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

/* INPUT_PULLDOWN availability.
 *
 * This CANNOT be detected with #ifdef. On the installed core (and in the Arduino
 * API generally) INPUT_PULLDOWN is an ENUMERATOR inside `enum PinMode`, not a
 * preprocessor macro, so `#ifdef INPUT_PULLDOWN` is always false even when the
 * constant exists. An earlier revision used #ifdef here and silently compiled out
 * every pull-down probe, which would have weakened the diagnosis on the real
 * board without any error.
 *
 * It was instead verified directly against the installed core 1.1.0:
 *   cores/arduino/api/Common.h:     INPUT_PULLDOWN = 0x3
 *   cores/arduino/wiring_digital.cpp: maps it to GPIO_PULL_DOWN
 * so it is a real internal pull-down, not a no-op. Used unconditionally on that
 * basis. If a future core ever removed it, the build would fail loudly here,
 * which is the correct outcome rather than degrading in silence. */
#define PT_HAS_PULLDOWN 1

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
#if PT_HAS_PULLDOWN
  logStep("W0: probing MISO with both internal pull-up AND pull-down.");
  logStep("    (INPUT_PULLDOWN verified present in core api/Common.h; it is an");
  logStep("     enum value, so it cannot be feature-tested with #ifdef.)");
#else
  logStep("W0: INPUT_PULLDOWN unavailable on this core - pull-up probing only.");
  logStep("    The unpowered-module case is still detectable, but a driven-high");
  logStep("    line cannot be confirmed without a pull-down.");
#endif

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

  /* ── Derived evidence ─────────────────────────────────────────────────────
   * Computed before the W2 printout so the operator sees the conclusion the
   * verdict will be based on, not just the raw counts. These same booleans drive
   * the W3 decision below.
   *
   * hiFollowsPull: with SS high (chip deselected) the line tracks the internal
   * pull, which is what a healthy high-Z MISO does. A line that ignores the pull
   * is being held by something else.
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

  /* Is the line DRIVEN when SS is low? The robust test is pull-independence: a
   * chip actively driving MISO has an output impedance far below the STM32's
   * ~40k internal pull, so the reading stops depending on which pull is fitted.
   * A line that is still high-Z keeps following the pull (high with pull-up, low
   * with pull-down).
   *
   * IMPORTANT CAVEAT: the MFRC522 clocks in an address byte before it drives
   * MISO with register data, so with SS held low and NO clock running the chip
   * may legitimately leave MISO high-Z. A false here is therefore NOT proof of a
   * fault; it only means this static probe could not confirm drive. That is why
   * the verdict below treats "follows pull, no drive seen" as inconclusive and
   * defers to Phase B, which clocks a real transaction. */
  bool loDriven;
#if PT_HAS_PULLDOWN
  loDriven = (loPuOnes == loPdOnes);
#else
  /* Without a pull-down the only evidence of drive is a change from the
   * deselected reading: high when deselected, low when selected. */
  loDriven = (loPuOnes == 0) && (hiPuOnes == hiPuS);
#endif

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
#if PT_HAS_PULLDOWN
  logStep("    SS LOW,  pull-down : ");
  ptLogf("      %s", ptWClassify(loPdOnes, loPdS));
#endif
  ptLogf("    deselected line follows the pull (healthy high-Z): %s",
         hiFollowsPull ? "YES" : "no");
  ptLogf("    selected line is actively driven (pull-independent): %s",
         loDriven ? "YES" : "not confirmed");

  /* ── Decision ─────────────────────────────────────────────────────────────
   * The combinations below are the ones that actually distinguish the suspects.
   * Each branch states the conclusion AND the next action, because a diagnosis
   * the operator cannot act on is only half useful.
   */
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
  } else if (hiFollowsPull && loDriven) {
    ptMatrixShowText2("OK");
    recordResult("W-miso", PT_PASS,
                 "Hi-Z when deselected, driven when selected");
    logStep("W3: DECISION - the line behaves correctly.");
    logStep("    Deselected it follows the pull, so the module releases MISO as");
    logStep("    it should. Selected it stops following the pull, so something on");
    logStep("    the other end is actively driving it.");
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
    ptMatrixShowText2("W");
    recordResult("W-miso", PT_VISUAL,
                 "Line healthy high-Z, but drive not confirmed - run Phase B");
    logStep("W3: DECISION - INCONCLUSIVE, and that is expected. Read carefully.");
    logStep("    When deselected, MISO follows the internal pull, so the line is");
    logStep("    healthy: it is NOT stuck low (no unpowered-module ESD clamp, no");
    logStep("    short to GND) and NOT stuck high (no short to 3V3/IOREF).");
    logStep("");
    logStep("    But this static probe could not confirm the module DRIVES MISO");
    logStep("    when selected. That is NOT necessarily a fault: the MFRC522");
    logStep("    clocks in an address byte before it drives MISO, so with SS low");
    logStep("    and no clock running it may legitimately leave the line high-Z.");
    logStep("");
    logStep("    => The electrical layer looks healthy. The real test is Phase B,");
    logStep("       which clocks an actual transaction. Run Phase B next:");
    logStep("         tools/run_phase.sh B");
    logStep("       If Phase B reads a valid fingerprint, you are done. If it");
    logStep("       still reads 0x00, THEN suspect, in order:");
    logStep("       1. Header pins not soldered (pressed-in pins look connected");
    logStep("          but are not - the most common RC522 fault).");
    logStep("       2. MISO and MOSI swapped, or the MISO jumper not seated.");
    logStep("       3. SCK not reaching the module, so it never clocks out data.");
    logStep("       4. Module damaged, for example by 5V on VCC.");
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

  /* Restore D12 to its SPI role. The probes above reconfigured it as a plain
   * GPIO input, and leaving it that way would break every later phase.
   *
   * A single SPI.begin() is sufficient and was verified against the installed
   * core: ZephyrSPI::begin() unconditionally calls init_dev_apply_pinctrl(),
   * whose own comment says it re-applies the DEFAULT pinctrl "so shared pins are
   * remuxed back to SPI after other peripherals have used them". There is no
   * already-initialised guard, so re-calling it always remuxes D11/D12/D13. An
   * earlier revision called begin(), then pinMode(MISO, INPUT), then begin()
   * again; the middle step was pointless because the final begin() overrode it.
   * Phase L passing on real hardware is the empirical proof this remux works. */
  logStep("");
  logStep("W4: restoring D11/D12/D13 to their SPI function");
  ptSpiBusBegin();
  logStep("    SPI.begin() re-applied the pinctrl (ZephyrSPI::begin calls");
  logStep("    init_dev_apply_pinctrl), remuxing D12 back to SPI MISO.");

  logStep("");
  logStep("Phase W complete.");
}

#endif /* PHASE_W_LINESTATE_H */
