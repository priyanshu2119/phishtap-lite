/*
 * PhishTap Lite — hw_selftest
 *
 * PHASE L — SPI loopback. RC522 DISCONNECTED, one jumper from D11 to D12.
 *
 * WHAT THIS PROVES
 *   Phase B read 0x00 from VersionReg at 100 kHz, 1 MHz and 4 MHz. Because the
 *   result did not change with clock speed, this is not a signal-integrity
 *   problem; MISO simply sits at logic 0 for the whole transfer. That is
 *   consistent with an unpowered module, a chip that is never selected, a missing
 *   or swapped MISO wire, a short to ground, or a dead chip. It is ALSO consistent
 *   with SPI2 not actually being routed to D11/D12/D13 on this core.
 *
 *   Loopback separates those two families. With MOSI wired straight to MISO and
 *   no slave involved, whatever the peripheral shifts out must come back in. If it
 *   does, the bus and the pinmux are fine and every remaining suspect is on the
 *   RC522 side. If it does not, the RC522 is irrelevant and the investigation
 *   moves to pinctrl, the deferred-init flag, the SPI object, or D10 GPIO control.
 *
 *   The board's variant.h defines MOSI D11 (PB15), MISO D12 (PB14), SCK D13
 *   (PB13) and SS D10 (PB9), and the devicetree overlay enables SPI2 with
 *   zephyr,deferred-init and st,gpio-control. That is strong evidence the pin map
 *   is right, but evidence from a header is not evidence from silicon, and this
 *   phase costs one jumper to settle.
 *
 * MATRIX CODES: shows "L" while running, "OK" or "ER" at the end.
 */

#ifndef PHASE_L_LOOPBACK_H
#define PHASE_L_LOOPBACK_H

#include <Arduino.h>
#include <SPI.h>

#include "pt_pins.h"
#include "pt_log.h"
#include "pt_matrix.h"
#include "pt_prompt.h"
#include "pt_spi_probe.h"

/*
 * Test patterns.
 *
 * The mix is deliberate. 0xA5 and 0x5A alternate bits, so they exercise every
 * transition and catch a line that only ever drives one polarity. 0x3C has an
 * interior run of ones bounded by zeros, which catches a stuck bit in the middle
 * of the word. The incrementing bytes catch a bus that echoes only part of a
 * sequence.
 *
 * 0x00 and 0xFF are included because they are the two values a stuck line
 * produces, and seeing them come back is only meaningful in the context of the
 * others. On their own they prove nothing: a dead MISO held low "passes" a 0x00
 * echo, and a dead MISO held high "passes" 0xFF. The decision logic below
 * therefore treats them as non-discriminating.
 */
static const uint8_t PT_LB_PATTERNS[] = {
  0xA5, 0x5A, 0xFF, 0x00, 0x3C,
  0x01, 0x02, 0x04, 0x08, 0x10,   /* incrementing single-bit walk */
  0x20, 0x40, 0x80, 0xDE, 0xAD
};
#define PT_LB_PATTERN_COUNT (sizeof(PT_LB_PATTERNS) / sizeof(PT_LB_PATTERNS[0]))

/* Each pattern set is repeated this many times per clock. A single clean pass can
 * happen by luck on a flaky dupont connection; three passes that disagree is a
 * contact problem and looks completely different from three identical passes. */
#define PT_LB_REPEATS 3

/* A pattern is only informative if a stuck line could not have produced it by
 * accident. 0x00 and 0xFF are excluded for that reason. */
static inline bool ptLbIsDiscriminating(uint8_t v) {
  return (v != 0x00) && (v != 0xFF);
}

typedef struct {
  uint8_t sent[PT_LB_PATTERN_COUNT];
  uint8_t got[PT_LB_PATTERN_COUNT];
  uint8_t matchAll;        /* every pattern echoed                    */
  uint8_t matchDiscrim;    /* every discriminating pattern echoed     */
  uint8_t discrimCount;    /* how many discriminating patterns exist  */
  uint8_t allZero;         /* every received byte was 0x00            */
  uint8_t allOnes;         /* every received byte was 0xFF            */
} PtLbRun;

/*
 * Send the whole pattern set once and score the result.
 *
 * Chip select is driven manually even though there is no slave to select. Two
 * reasons: the handoff specifies it, and it exercises D10 as a GPIO output, which
 * matters because the overlay declares st,gpio-control. If D10 could not be
 * driven, the RC522 would never be selected and that alone would explain a dead
 * MISO.
 */
static void ptLbRunOnce(uint32_t clockHz, PtLbRun* r) {
  r->matchAll = 1;
  r->matchDiscrim = 1;
  r->discrimCount = 0;
  r->allZero = 1;
  r->allOnes = 1;

  for (uint8_t i = 0; i < PT_LB_PATTERN_COUNT; ++i) {
    uint8_t out = PT_LB_PATTERNS[i];
    uint8_t in  = ptSpiXfer(out, clockHz);
    r->sent[i] = out;
    r->got[i]  = in;

    if (in != out) r->matchAll = 0;
    if (in != 0x00) r->allZero = 0;
    if (in != 0xFF) r->allOnes = 0;

    if (ptLbIsDiscriminating(out)) {
      r->discrimCount++;
      if (in != out) r->matchDiscrim = 0;
    }
  }
}

/*
 * Interpret one clock's results and print the reasoning.
 *
 * R15: the raw table is always printed first, then the decision. A verdict
 * without the values behind it cannot be argued with, and the values are what
 * makes an intermittent fault distinguishable from a dead line.
 */
static PtMisoVerdict ptLbInterpret(uint32_t clockHz, const PtLbRun* runs,
                                   uint8_t nRuns) {
  /* Do the repeats agree with each other? Disagreement is the signature of a
   * contact problem, and it is invisible in any single run. */
  bool consistent = true;
  for (uint8_t i = 0; i < PT_LB_PATTERN_COUNT && consistent; ++i) {
    for (uint8_t k = 1; k < nRuns; ++k) {
      if (runs[k].got[i] != runs[0].got[i]) { consistent = false; break; }
    }
  }

  bool allEchoed = true;
  bool allZero   = true;
  bool allOnes   = true;
  for (uint8_t k = 0; k < nRuns; ++k) {
    if (!runs[k].matchDiscrim) allEchoed = false;
    if (!runs[k].allZero)      allZero   = false;
    if (!runs[k].allOnes)      allOnes   = false;
  }

  ptLogf("    @ %7lu Hz  discriminating matches: %s  repeats identical: %s",
         (unsigned long)clockHz, allEchoed ? "ALL" : "not all",
         consistent ? "yes" : "NO");

  if (!consistent) {
    logStep("      -> INTERMITTENT. Repeats disagree, so the line makes and");
    logStep("         breaks contact. Re-seat the D11-D12 jumper.");
    return PT_MISO_NOISY;
  }
  if (allEchoed) {
    logStep("      -> ECHO OK. MOSI reaches MISO at this clock.");
    return PT_MISO_OK;
  }
  if (allZero) {
    logStep("      -> STUCK LOW. Every byte came back 0x00 whatever was sent.");
    return PT_MISO_STUCK_LOW;
  }
  if (allOnes) {
    logStep("      -> STUCK HIGH. Every byte came back 0xFF whatever was sent.");
    return PT_MISO_STUCK_HIGH;
  }
  logStep("      -> PARTIAL. Some bits echo and some do not; see the table.");
  return PT_MISO_NOISY;
}

inline void phaseL_run(void) {
  logSection("PHASE L - SPI LOOPBACK (RC522 DISCONNECTED)");

  ptMatrixShowText2("L");

  /* R16: announce the wiring change on the console and on the matrix, then wait.
   * No tap poll is offered here because the reader is disconnected by
   * definition, so there is nothing to tap. The countdown is the only path. */
  ptWaitForWiringNoTap(
      "L",
      "REMOVE ALL RC522 WIRES. FIT ONE JUMPER: D11 (MOSI) TO D12 (MISO).");

  logStep("");
  logStep("L1: driving D10 (SS) low by hand and echoing patterns MOSI -> MISO");
  logStep("    Pattern set: 0xA5 0x5A 0xFF 0x00 0x3C then a single-bit walk");
  logStep("    0x00 and 0xFF are NOT counted as proof of anything, because a");
  logStep("    stuck line reproduces them by accident.");
  logStep("");

  /* The bus is brought up fresh. If a previous phase already called SPI.begin()
   * this is harmless, and calling it here means Phase L can run on its own with
   * no other phase enabled, which is how it will normally be used. */
  ptSpiBusBegin();

  /* Prove D10 can actually be driven, since the overlay declares chip select as a
   * GPIO. If the pinmux had claimed it for the peripheral, digitalWrite would be
   * ignored and a slave would never be selected. */
  digitalWrite(PT_RC522_SS, LOW);
  delay(2);
  digitalWrite(PT_RC522_SS, HIGH);
  logStep("    D10 driven low then high as a plain GPIO (st,gpio-control).");

  PtMisoVerdict perClock[PT_PROBE_CLOCK_COUNT];
  uint8_t okClocks = 0;

  for (uint8_t c = 0; c < PT_PROBE_CLOCK_COUNT; ++c) {
    uint32_t clockHz = PT_PROBE_CLOCKS[c];
    PtLbRun runs[PT_LB_REPEATS];

    ptLogf("  --- clock %lu Hz, %d repeats ---",
           (unsigned long)clockHz, (int)PT_LB_REPEATS);

    for (uint8_t k = 0; k < PT_LB_REPEATS; ++k) {
      ptLbRunOnce(clockHz, &runs[k]);
      ptLogf("    repeat %u:", (unsigned)(k + 1));
      ptPrintXferTable(runs[k].sent, runs[k].got, PT_LB_PATTERN_COUNT);
    }

    perClock[c] = ptLbInterpret(clockHz, runs, PT_LB_REPEATS);
    if (perClock[c] == PT_MISO_OK) okClocks++;
    logStep("");
  }

  /* ── Overall decision ────────────────────────────────────────────────────
   * The three outcomes map onto three completely different investigations, so
   * the summary says which one to start rather than just reporting a failure.
   */
  logStep("L2: DECISION");

  if (okClocks == PT_PROBE_CLOCK_COUNT) {
    ptMatrixShowText2("OK");
    recordResult("L-loopback", PT_PASS, "All patterns echoed at all 3 clocks");
    logStep("    SPI2 is correctly routed to D11/D12/D13 and D10 drives as a");
    logStep("    GPIO. The peripheral, the pinmux and the board are all fine.");
    logStep("");
    logStep("    => The fault is on the RC522 side. In order of likelihood:");
    logStep("       1. Module not powered. VCC must be on the 3V3 header pin.");
    logStep("          Do the LED power test in the README first to prove which");
    logStep("          physical pin is really 3V3 and which is GND.");
    logStep("       2. One-pin-off on JANALOG. That header is BOOT, IOREF, RESET,");
    logStep("          +3V3, +5V, GND, GND, VIN. Landing on RESET reboots the MCU,");
    logStep("          BOOT enters the bootloader, and +5V destroys the RC522,");
    logStep("          which is a 2.5-3.3V part.");
    logStep("       3. SS/SDA not reaching the chip, so it never drives MISO.");
    logStep("       4. MISO and MOSI swapped, or the MISO jumper not seated.");
    logStep("       5. MISO shorted to GND.");
    logStep("       6. Module damaged, for example by 5V on VCC.");
    logStep("");
    logStep("    Next: run Phase W with the RC522 wired to narrow this down.");
  } else if (okClocks == 0) {
    bool allLow  = (perClock[0] == PT_MISO_STUCK_LOW  &&
                    perClock[1] == PT_MISO_STUCK_LOW  &&
                    perClock[2] == PT_MISO_STUCK_LOW);
    bool allHigh = (perClock[0] == PT_MISO_STUCK_HIGH &&
                    perClock[1] == PT_MISO_STUCK_HIGH &&
                    perClock[2] == PT_MISO_STUCK_HIGH);

    ptMatrixShowText2("ER");

    if (allLow || allHigh) {
      recordResult("L-loopback", PT_FAIL,
                   allLow ? "No echo at any clock, MISO stuck LOW"
                          : "No echo at any clock, MISO stuck HIGH");
      logStep("    Nothing echoed at ANY clock, and the line is pinned to one");
      logStep("    level. Since the loopback jumper bypasses the RC522 entirely,");
      logStep("    this is NOT a module problem. Investigate, in order:");
      logStep("      1. Is the D11-D12 jumper actually fitted and seated? This is");
      logStep("         the most common cause and takes ten seconds to check.");
      logStep("      2. Are D11 and D12 the pins you think? D11 is MOSI (PB15) and");
      logStep("         D12 is MISO (PB14) per variant.h. Counting from the wrong");
      logStep("         end of the header is easy to do.");
      logStep("      3. Is SPI.begin() reaching SPI2? The overlay sets");
      logStep("         zephyr,deferred-init, so the bus is only initialised when");
      logStep("         SPI.begin() runs. If the SPI object bound to a different");
      logStep("         controller, these pins would never be driven.");
      logStep("      4. pinctrl: confirm the SPI2 pins are muxed to the");
      logStep("         peripheral and not left as plain GPIO.");
      logStep("    A stuck-HIGH result additionally suggests a pull-up is winning,");
      logStep("    which points at the pin never being driven at all.");
    } else {
      recordResult("L-loopback", PT_FAIL, "Stable but WRONG bytes - second driver on MISO?");
      logStep("    The received bytes are STABLE across repeats but do not match");
      logStep("    what was sent. A loose jumper gives random values; a consistent");
      logStep("    wrong value means something else is driving MISO.");
      logStep("");
      logStep("    SUSPECT FIRST: the RC522 is still connected. This phase needs it");
      logStep("    COMPLETELY removed, all seven wires, leaving only the D11-D12");
      logStep("    jumper. Counterfeit RC522 modules are documented as driving MISO");
      logStep("    even when their chip select is not asserted (miguelbalboa/rfid");
      logStep("    issue #428), so a connected module corrupts the loopback even");
      logStep("    though it is deselected.");
      logStep("");
      logStep("    If the RC522 really is fully disconnected, then suspect the");
      logStep("    D11-D12 jumper itself, or that MOSI is not toggling.");
    }
  } else {
    ptMatrixShowText2("ER");
    recordResult("L-loopback", PT_FAIL, "Echo works at some clocks but not all");
    ptLogf("    %u of %u clocks echoed cleanly.", (unsigned)okClocks,
           (unsigned)PT_PROBE_CLOCK_COUNT);
    logStep("    A clock-dependent result on a direct jumper is unusual and");
    logStep("    points at marginal contact rather than a configuration fault.");
    logStep("    Note WHICH clocks passed and re-run once with re-seated jumpers");
    logStep("    before drawing a conclusion.");
  }

  logStep("");
  logStep("Phase L complete. Remove the D11-D12 jumper before Phase W or B.");
}

#endif /* PHASE_L_LOOPBACK_H */
