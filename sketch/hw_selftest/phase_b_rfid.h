/*
 * PhishTap Lite — hw_selftest
 *
 * PHASE B — MFRC522 / RC522 over SPI. Seven jumper wires.
 *
 * This is the highest-risk phase in the whole bring-up and it runs BEFORE the
 * LEDs and the buzzer on purpose. Everything else on this board is either
 * soldered down (matrix, RGB LEDs) or a single GPIO with a transistor (LEDs,
 * buzzer), so a failure there is a wiring fault you can see. The RC522 is the
 * only peripheral that depends on a third-party library compiling and behaving
 * correctly against the Zephyr core, and on four SPI lines being right at once.
 * If it does not work, the product does not work, and no amount of working LEDs
 * changes that. Find out first.
 *
 * ============================================================================
 * NO-WRITE POLICY
 * ============================================================================
 * This firmware is STRICTLY READ-ONLY towards contactless cards. No file in this
 * project shall ever call MIFARE_Write, MIFARE_Ultralight_Write, MIFARE_SetUid,
 * MIFARE_OpenUidBackdoor, MIFARE_UnbrickUidSector, MIFARE_SetAccessBits,
 * MIFARE_Increment, MIFARE_Decrement, MIFARE_Restore, MIFARE_Transfer,
 * MIFARE_SetValue, PCD_NTAG216_AUTH, or any other function that modifies card
 * memory, nor shall it send the raw PICC_CMD_MF_WRITE / PICC_CMD_UL_WRITE /
 * PICC_CMD_MF_INCREMENT / PICC_CMD_MF_DECREMENT / PICC_CMD_MF_RESTORE /
 * PICC_CMD_MF_TRANSFER command codes.
 *
 * Compliance is audited by a grep-based CI script, tools/no_write_check.sh,
 * which strips comments before scanning so this very paragraph does not trip it.
 * A compile-time guard is not possible: the library declares these as ordinary
 * public members, so referencing one is always legal C++.
 *
 * Reading a UID needs only PICC_IsNewCardPresent, PICC_ReadCardSerial,
 * PICC_HaltA and PCD_StopCrypto1. Nothing else.
 * ============================================================================
 */

#ifndef PHASE_B_RFID_H
#define PHASE_B_RFID_H

#include <Arduino.h>
#include <SPI.h>

/*
 * MFRC522_SPICLOCK CANNOT be overridden from a sketch. Do not try.
 *
 * An earlier revision of this file defined it to 1 MHz before including the
 * header, on the reasoning that 4 MHz is marginal over dupont jumpers. That was
 * wrong, and the reason is worth recording:
 *
 *   MFRC522.h:21   #ifndef MFRC522_SPICLOCK
 *   MFRC522.h:22   #define MFRC522_SPICLOCK (4000000u)
 *   MFRC522.cpp:49,65,82,106   SPI.beginTransaction(SPISettings(MFRC522_SPICLOCK, ...))
 *
 * Every single SPI transaction the library performs lives in MFRC522.cpp, which
 * is its own translation unit and includes MFRC522.h WITHOUT our define. The
 * macro therefore always expands to the 4 MHz default there. Defining it in the
 * sketch changes nothing at all - it only affects code in our own translation
 * unit, and we issue no SPI transactions of our own through the library.
 *
 * The define is kept (harmlessly) so the intent is documented. The real
 * slow-clock test is performed by ptSpiReadReg() in pt_spi_probe.h, which drives
 * the bus directly and can therefore use any clock it likes.
 */
#ifndef MFRC522_SPICLOCK
#define MFRC522_SPICLOCK 1000000u
#endif

#include <MFRC522.h>

#include "pt_pins.h"
#include "pt_log.h"
#include "pt_spi_probe.h"

static MFRC522 s_rfid(PT_RC522_SS, PT_RC522_RST);

/* Set once Phase B has proven the reader works. Later phases use a card tap as
 * the human-confirmation input, and they need to know whether that is available
 * or whether they must fall back to "judge this by eye". */
static bool s_rfidUsable = false;

/*
 * Raw SPI access lives in pt_spi_probe.h, shared with Phase L and Phase W so the
 * three phases cannot drift apart in how they drive the bus. The local names are
 * kept as thin aliases because the rest of this file predates the extraction and
 * the shorter names read better at the call sites.
 *
 * WHY RAW ACCESS EXISTS AT ALL: when a reader does not respond there are two very
 * different explanations, the bus is not working or the bus works and the chip is
 * unhappy. Going through the library cannot distinguish them, because a failure
 * looks identical either way.
 */
inline void    rfidHardReset(void) { ptRfidHardReset(); }
inline uint8_t rfidRawReadReg(uint8_t rawAddr, uint32_t clockHz) {
  return ptSpiReadReg(rawAddr, clockHz);
}

#define PT_RC522_REG_VERSION PT_REG_VERSION

/*
 * B0a: read the whole post-reset fingerprint and compare it against the values
 * the NXP datasheet documents.
 *
 * WHY MORE THAN VersionReg: a single register can read 0x00 for reasons that
 * have nothing to do with the chip being absent, and a single register reading
 * correctly can happen by accident on a noisy line. Ten registers with ten
 * different documented defaults form a fingerprint. Getting all ten right by
 * chance on a broken bus is not a realistic possibility, so a full match is
 * strong positive evidence and a full 0x00 row is strong negative evidence.
 *
 * These must be read BEFORE PCD_Init(), because PCD_Init overwrites several of
 * them: ModeReg goes from its 0x3F reset value to 0x3D, and RFCfgReg from 0x48
 * to 0x70 when PCD_SetAntennaGain(RxGain_max) runs. Reading afterwards would
 * compare against the wrong expectations.
 *
 * Returns the number of registers whose masked bits matched.
 */
inline uint8_t rfidCheckFingerprint(uint32_t clockHz) {
  uint8_t matched = 0;

  logStep("      register        addr   expected   actual   match");
  for (uint8_t i = 0; i < PT_FINGERPRINT_COUNT; ++i) {
    const PtRegExpect* e = &PT_RESET_FINGERPRINT[i];
    uint8_t actual = rfidRawReadReg(e->addr, clockHz);
    bool ok = ((actual & e->mask) == (e->expected & e->mask));
    if (ok) matched++;
    ptLogf("      %-16s 0x%02X   0x%02X       0x%02X     %s",
           e->name, e->addr, e->expected, actual, ok ? "yes" : "NO");
  }
  ptLogf("      %u of %u registers match their documented reset value",
         (unsigned)matched, (unsigned)PT_FINGERPRINT_COUNT);
  return matched;
}

/*
 * B0b: read VersionReg many times and report how many reads agreed.
 *
 * WHY: this separates a STUCK line from a NOISY one, which the single reads
 * above cannot do. A line held low returns the same byte twenty times out of
 * twenty. A marginal connection returns a mix. The two have completely different
 * fixes, so the count is printed rather than collapsed into pass or fail.
 */
inline void rfidRepeatRead(uint8_t rawAddr, uint32_t clockHz, uint8_t repeats) {
  uint8_t first = rfidRawReadReg(rawAddr, clockHz);
  uint8_t same = 1;
  uint8_t distinct = 1;
  uint8_t seen[8];
  seen[0] = first;

  for (uint8_t i = 1; i < repeats; ++i) {
    uint8_t v = rfidRawReadReg(rawAddr, clockHz);
    if (v == first) same++;
    bool known = false;
    for (uint8_t k = 0; k < distinct && k < 8; ++k) {
      if (seen[k] == v) { known = true; break; }
    }
    if (!known && distinct < 8) seen[distinct++] = v;
    delay(2);
  }

  ptLogf("      %u reads of 0x%02X: %u identical to the first (0x%02X), %u distinct value(s)",
         (unsigned)repeats, rawAddr, (unsigned)same, first, (unsigned)distinct);
  logStep("      values seen:");
  for (uint8_t k = 0; k < distinct; ++k) ptLogf("        0x%02X", seen[k]);

  if (same == repeats) {
    logStep("      -> PERFECTLY STABLE. The line is not noisy. If the value is");
    logStep("         wrong it is wrong consistently, which means stuck or");
    logStep("         unpowered rather than a marginal connection.");
  } else {
    logStep("      -> VARIES BETWEEN READS. That is a contact or noise problem,");
    logStep("         not a dead line. Re-seat jumpers and shorten them.");
  }
}

/*
 * B0c: compare manual chip select against the library's own handling.
 *
 * WHY: the devicetree overlay sets st,gpio-control on SPI2, meaning chip select
 * is a plain GPIO rather than a peripheral function. If that assumption is wrong
 * for this core build, the library's transaction handling and a manual
 * digitalWrite would disagree, and only one of the two would actually select the
 * chip. Running the same read both ways settles it without needing to read the
 * overlay.
 */
inline void rfidCompareChipSelect(uint8_t rawAddr, uint32_t clockHz) {
  /* Manual: drive SS ourselves around a raw transaction. */
  uint8_t manual = rfidRawReadReg(rawAddr, clockHz);

  /* Library: let MFRC522 drive its own _chipSelectPin inside PCD_ReadRegister. */
  uint8_t viaLib = s_rfid.PCD_ReadRegister(
      static_cast<MFRC522::PCD_Register>(rawAddr << 1));

  ptLogf("      manual SS   -> 0x%02X", manual);
  ptLogf("      library SS  -> 0x%02X", viaLib);
  if (manual == viaLib) {
    logStep("      -> Both paths agree, so chip select is being driven correctly");
    logStep("         and st,gpio-control behaves as the overlay declares.");
  } else {
    logStep("      -> The two paths DISAGREE. Chip select handling is the fault.");
    logStep("         Whichever value is a plausible register default is the");
    logStep("         path that actually selected the chip.");
  }
}


/* ── Confirmation protocol ──────────────────────────────────────────────────
 * The matrix, the LEDs and the buzzer have no readback, so several checks can
 * only be confirmed by a human. Monitor input is deliberately NOT used for this:
 * whether the virtual serial channel can carry keystrokes back to the MCU is not
 * something this project has verified, and a selftest that hangs waiting for
 * input that can never arrive is worse than no confirmation at all.
 *
 * A card tap is used instead. It is unambiguous, it needs no extra wiring
 * because Phase B already proved the reader, and it doubles as a second live
 * exercise of the RFID path.
 */
#define PT_CONFIRM_NO  0   /* timed out: the operator did not confirm */
#define PT_CONFIRM_YES 1   /* card tapped: the operator confirmed     */
#define PT_CONFIRM_NA  2   /* no reader: a human must judge it offline */

/*
 * Wait for one card tap.
 *
 * F7 lesson 1: PICC_IsNewCardPresent() sends REQA and already treats
 * STATUS_COLLISION as "a card answered", so a collision is a hit and not an
 * error. Do not reinterpret it here.
 *
 * F7 lesson 2: never send a second REQA before SELECT. PICC_ReadCardSerial()
 * performs the SELECT, so it is called exactly once per detected card and the
 * card is halted immediately afterwards. Re-polling a card that is already in
 * READY state is what produced the anticollision failures in the previous build.
 */
inline bool rfidWaitTap(uint32_t timeoutMs) {
  uint32_t started = millis();
  while (millis() - started < timeoutMs) {
    if (s_rfid.PICC_IsNewCardPresent()) {
      if (s_rfid.PICC_ReadCardSerial()) {
        s_rfid.PICC_HaltA();
        s_rfid.PCD_StopCrypto1();
        return true;
      }
      /* Detected but SELECT failed. Halt and retry rather than leaving the card
       * in READY state, where the next REQA would be ignored. */
      s_rfid.PICC_HaltA();
    }
    delay(40);
  }
  return false;
}

/*
 * Wait for the card to be taken away.
 *
 * WHY WUPA and not REQA: after PICC_HaltA() the card sits in HALT state and no
 * longer answers REQA, so PICC_IsNewCardPresent() reports "absent" even while
 * the card is still lying on the reader. WUPA wakes a halted card, which makes
 * it the only correct presence probe here: STATUS_OK or STATUS_COLLISION means
 * still there, anything else means it has gone.
 *
 * The card is re-halted after each successful probe so every probe starts from
 * the same state and the result does not depend on how many times we polled.
 */
inline bool rfidWaitRemoval(uint32_t timeoutMs) {
  uint32_t started = millis();
  uint8_t atqa[2];
  while (millis() - started < timeoutMs) {
    uint8_t size = sizeof(atqa);
    MFRC522::StatusCode sc = s_rfid.PICC_WakeupA(atqa, &size);
    if (sc != MFRC522::STATUS_OK && sc != MFRC522::STATUS_COLLISION) {
      return true;
    }
    s_rfid.PICC_HaltA();
    delay(120);
  }
  return false;
}

/* Ask the operator to confirm what they just saw or heard. */
inline unsigned char ptConfirm(const char* question, uint32_t timeoutMs) {
  if (!s_rfidUsable) {
    logStep(question);
    logStep("    Reader unavailable - cannot capture a confirmation.");
    logStep("    Judge this step yourself and note the result.");
    delay(2500);
    return PT_CONFIRM_NA;
  }
  logStep(question);
  ptLogf("    TAP THE CARD within %lus if that was CORRECT.",
         (unsigned long)(timeoutMs / 1000));
  logStep("    Do NOT tap if it was wrong - let it time out.");
  bool tapped = rfidWaitTap(timeoutMs);
  logStep(tapped ? "    Tap received." : "    Timed out with no tap.");
  return tapped ? PT_CONFIRM_YES : PT_CONFIRM_NO;
}

inline PtVerdict verdictFromConfirm(unsigned char c) {
  if (c == PT_CONFIRM_YES) return PT_PASS;
  if (c == PT_CONFIRM_NO)  return PT_FAIL;
  return PT_VISUAL;
}

/* ── Library type names, re-implemented ─────────────────────────────────────
 * MFRC522::GetStatusCodeName() and PICC_GetTypeName() both return a
 * __FlashStringHelper*, which is an AVR PROGMEM abstraction. Streaming that
 * through the Bridge-backed Monitor on an ARM/Zephyr target is not something
 * this project needs to bet a bring-up on, so the mappings are duplicated here
 * against plain const char*. The cost is a few hundred bytes of flash.
 */
inline const char* ptStatusCodeName(MFRC522::StatusCode sc) {
  switch (sc) {
    case MFRC522::STATUS_OK:              return "OK";
    case MFRC522::STATUS_ERROR:           return "ERROR";
    case MFRC522::STATUS_COLLISION:       return "COLLISION";
    case MFRC522::STATUS_TIMEOUT:         return "TIMEOUT";
    case MFRC522::STATUS_NO_ROOM:         return "NO_ROOM";
    case MFRC522::STATUS_INTERNAL_ERROR:  return "INTERNAL_ERROR";
    case MFRC522::STATUS_INVALID:         return "INVALID";
    case MFRC522::STATUS_CRC_WRONG:       return "CRC_WRONG";
    case MFRC522::STATUS_MIFARE_NACK:     return "MIFARE_NACK";
  }
  return "UNKNOWN";
}

inline const char* ptPiccTypeName(MFRC522::PICC_Type t) {
  switch (t) {
    case MFRC522::PICC_TYPE_UNKNOWN:       return "UNKNOWN";
    case MFRC522::PICC_TYPE_ISO_14443_4:   return "ISO-14443-4";
    case MFRC522::PICC_TYPE_ISO_18092:     return "ISO-18092/NFC";
    case MFRC522::PICC_TYPE_MIFARE_MINI:   return "MIFARE Mini";
    case MFRC522::PICC_TYPE_MIFARE_1K:     return "MIFARE 1K";
    case MFRC522::PICC_TYPE_MIFARE_4K:     return "MIFARE 4K";
    case MFRC522::PICC_TYPE_MIFARE_UL:     return "MIFARE Ultralight";
    case MFRC522::PICC_TYPE_MIFARE_PLUS:   return "MIFARE Plus";
    case MFRC522::PICC_TYPE_MIFARE_DESFIRE:return "MIFARE DESFire";
    case MFRC522::PICC_TYPE_TNP3XXX:       return "TNP3XXX";
    case MFRC522::PICC_TYPE_NOT_COMPLETE:  return "NOT_COMPLETE";
  }
  return "?";
}

/*
 * Interpret the VersionReg byte.
 *
 * This single register read is the definitive SPI test. It is a pure read with
 * no RF field involved, so it isolates the bus from the antenna: 0x00 means MISO
 * is stuck low or nothing is answering, 0xFF means MISO is stuck high or the
 * line is floating, and a real version byte means all four SPI lines plus RST
 * and SS are correct.
 */
inline const char* ptVersionName(uint8_t v) {
  switch (v) {
    case 0x88: return "FM17522 clone (Fudan) - works, but self-test may differ";
    case 0x90: return "MFRC522 v0.0";
    case 0x91: return "MFRC522 v1.0";
    case 0x92: return "MFRC522 v2.0";
    case 0x00: return "READ 0x00 - MISO stuck low, no SPI response";
    case 0xFF: return "READ 0xFF - MISO stuck high or floating";
  }
  return "unrecognised version byte";
}

inline void rfidFormatUid(char* out, size_t outLen) {
  size_t used = 0;
  out[0] = '\0';
  for (uint8_t i = 0; i < s_rfid.uid.size && used + 3 < outLen; ++i) {
    int n = snprintf(out + used, outLen - used, "%02X%s",
                     s_rfid.uid.uidByte[i],
                     (i + 1 < s_rfid.uid.size) ? " " : "");
    if (n <= 0) break;
    used += (size_t)n;
  }
}

inline void phaseB_run(void) {
  logSection("PHASE B - RC522 RFID READER (7 wires)");
  logStep("Wiring: VCC->3V3 (NEVER 5V), GND->GND, RST->D5, SDA/SS->D10,");
  logStep("        SCK->D13, MOSI->D11, MISO->D12. IRQ left unconnected.");
  logStep("");

  /* Bring the bus up exactly as the official UNO Q SPI example does: SS is a
   * plain output held HIGH (deselected) before SPI.begin() claims the pins. */
  pinMode(PT_RC522_SS, OUTPUT);
  digitalWrite(PT_RC522_SS, HIGH);
  SPI.begin();

  /* ── B0: raw SPI probing, before the library is involved at all ────────────
   * Four sub-tests, ordered so that each one narrows the fault:
   *   B0a  the post-reset register fingerprint, expected versus actual
   *   B0b  the same register read 20 times, to tell stuck from noisy
   *   B0c  manual chip select versus the library's, to test st,gpio-control
   *   B0d  the clock sweep, to test signal integrity
   *
   * All of this happens BEFORE PCD_Init(), deliberately. PCD_Init overwrites
   * ModeReg (0x3F becomes 0x3D) and RFCfgReg (0x48 becomes 0x70 via
   * PCD_SetAntennaGain), so reading the fingerprint afterwards would compare
   * against the wrong expectations.
   *
   * A hardware reset is forced first. See ptRfidHardReset() for why PCD_Init's
   * own reset cannot be relied upon.
   */
  ptSpiBusBegin();
  rfidHardReset();

  /* Everything in B0 uses the slowest clock. If the bus is marginal, 100 kHz
   * gives it the best chance, and a failure at 100 kHz is far more meaningful
   * than a failure at 4 MHz. */
  const uint32_t slowClock = PT_PROBE_CLOCKS[0];

  logStep("B0a: post-reset register fingerprint (raw SPI, library bypassed)");
  ptLogf("      clock %lu Hz, read immediately after a hardware reset",
         (unsigned long)slowClock);
  logStep("      Expected values are from the NXP MFRC522 data sheet Rev. 3.9,");
  logStep("      which documents each register's reset value explicitly.");
  uint8_t matched = rfidCheckFingerprint(slowClock);
  logStep("");

  logStep("B0b: stability - VersionReg read 20 times");
  rfidRepeatRead(PT_REG_VERSION, slowClock, 20);
  logStep("");

  logStep("B0c: chip select - manual GPIO versus the library's own handling");
  rfidCompareChipSelect(PT_REG_VERSION, slowClock);
  logStep("");

  logStep("B0d: clock sweep on VersionReg");
  uint8_t  rawVersion   = 0x00;
  uint32_t rawBestClock = 0;
  uint8_t  rawAllSame   = 1;
  uint8_t  rawFirst     = 0xFF;

  for (uint8_t i = 0; i < PT_PROBE_CLOCK_COUNT; ++i) {
    uint8_t v = rfidRawReadReg(PT_REG_VERSION, PT_PROBE_CLOCKS[i]);
    ptLogf("    @ %7lu Hz -> 0x%02X  (%s)",
           (unsigned long)PT_PROBE_CLOCKS[i], v, ptVersionName(v));
    if (i == 0) rawFirst = v;
    else if (v != rawFirst) rawAllSame = 0;
    /* Remember the slowest clock that produced a plausible version byte. */
    if (rawVersion == 0x00 && v != 0x00 && v != 0xFF) {
      rawVersion   = v;
      rawBestClock = PT_PROBE_CLOCKS[i];
    }
  }
  logStep("");

  /* ── B0 decision ─────────────────────────────────────────────────────────
   * The fingerprint carries more weight than VersionReg alone. A chip that
   * answers with ten correct documented defaults is unquestionably alive and
   * correctly wired; a chip that returns 0x00 for all ten is unquestionably not
   * talking, and no amount of retrying will change that.
   */
  logStep("B0 DECISION");
  if (matched == PT_FINGERPRINT_COUNT) {
    recordResult("B0-fingerprint", PT_PASS, "All registers match datasheet reset values");
    logStep("    Every register matches its documented reset value. The module is");
    logStep("    powered, selected, clocked and reading back correctly.");
  } else if (matched == 0) {
    recordResult("B0-fingerprint", PT_FAIL, "No register matched - bus or power dead");
    if (rawAllSame && rawFirst == 0x00) {
      logStep("    All ten registers read 0x00 and the line is stable, so MISO is");
      logStep("    held low rather than noisy. Run Phase W to tell an unpowered");
      logStep("    module from a short to ground, and Phase L to rule the board");
      logStep("    itself in or out.");
      logStep("");
      logStep("    Most likely first: the module is not powered. Do the LED power");
      logStep("    test in the README to prove which header pin is really 3V3.");
      logStep("    JANALOG order is BOOT, IOREF, RESET, +3V3, +5V, GND, GND, VIN,");
      logStep("    so one pin off lands on RESET, BOOT or +5V. The RC522 is a");
      logStep("    2.5-3.3V part and +5V will destroy it.");
    } else if (rawAllSame && rawFirst == 0xFF) {
      logStep("    All reads are 0xFF, so MISO is held high or floating. Check");
      logStep("    that MISO is actually in D12, and look for a jumper landing on");
      logStep("    IOREF, which is a 3V3 output two pins from 3V3.");
    } else {
      logStep("    Reads vary but none matches. Suspect intermittent contact.");
      logStep("    Re-seat every jumper, keep them under 20cm, and re-run.");
    }
  } else {
    recordResult("B0-fingerprint", PT_FAIL, "Some registers matched, some did not");
    ptLogf("    %u of %u matched. A partial match means the bus is working but",
           (unsigned)matched, (unsigned)PT_FINGERPRINT_COUNT);
    logStep("    some bits are corrupt, which points at signal integrity or a");
    logStep("    marginal connection rather than a dead module. Compare the");
    logStep("    expected and actual columns above: if the differences are all in");
    logStep("    the same bit position, suspect one specific data line.");
  }

  if (rawVersion != 0x00 && rawVersion != 0xFF) {
    ptLogf("    VersionReg resolved to 0x%02X, first working clock %lu Hz.",
           rawVersion, (unsigned long)rawBestClock);
    if (rawBestClock < 4000000u) {
      logStep("    NOTE: it only answered below 4 MHz. The library is fixed at");
      logStep("    4 MHz and cannot be slowed from a sketch, so reads may be");
      logStep("    unreliable. Shorten the jumpers and route them away from the");
      logStep("    antenna coil.");
    }
  }

  /* ── B1: the same read, this time through the library ─────────────────────
   * Reset again first: the raw probe left the chip selected and deselected
   * outside the library's expectations, and PCD_Init's own reset is conditional
   * on reading RST low (see rfidHardReset).
   */
  rfidHardReset();
  s_rfid.PCD_Init();

  ptLogf("    Library SPI clock is fixed at %lu Hz (not overridable from a sketch)",
         (unsigned long)MFRC522_SPICLOCK);

  uint8_t version = s_rfid.PCD_ReadRegister(MFRC522::VersionReg);
  ptLogf("B1: VersionReg via library = 0x%02X -> %s", version,
         ptVersionName(version));

  if (rawVersion != 0x00 && (version == 0x00 || version == 0xFF)) {
    logStep("    Raw SPI worked but the library read did not. That points at the");
    logStep("    library or its 4 MHz clock rather than the wiring - retry with");
    logStep("    shorter jumpers, and report this combination back.");
  }

  bool spiAlive = (version != 0x00 && version != 0xFF);
  if (!spiAlive) {
    recordResult("B1-spi", PT_FAIL, "No SPI response from the reader");
    logStep("");
    logStep("    The library could not read the chip either. Work through the");
    logStep("    B0 checklist above - it covers power, MISO/MOSI, SS, SCK, RST,");
    logStep("    solder joints and jumper routing.");
    s_rfidUsable = false;
    recordResult("B2-selftest", PT_SKIP, "Skipped: no SPI response");
    recordResult("B3-uid", PT_SKIP, "Skipped: no SPI response");
    recordResult("B4-stable", PT_SKIP, "Skipped: no SPI response");
    recordResult("B5-removal", PT_SKIP, "Skipped: no SPI response");
    return;
  }
  recordResult("B1-spi", PT_PASS, ptVersionName(version));

  /* ── B1b: prove that WRITES reach the chip, not just reads ─────────────────
   * PCD_Init() has just run, and it writes known values to registers whose reset
   * defaults we already read in B0a. ModeReg goes from 0x3F to 0x3D, and
   * RFCfgReg goes from 0x48 to 0x70 because PCD_Init calls
   * PCD_SetAntennaGain(RxGain_max).
   *
   * A bus can read correctly and still fail to write, for example if MOSI is
   * marginal while MISO is fine. Comparing the post-init values against both the
   * reset default and the value PCD_Init should have written catches that, and it
   * costs two register reads.
   */
  uint8_t modeAfter  = s_rfid.PCD_ReadRegister(MFRC522::ModeReg);
  uint8_t rfcfgAfter = s_rfid.PCD_ReadRegister(MFRC522::RFCfgReg);
  logStep("");
  logStep("B1b: register values after PCD_Init(), to prove writes land");
  logStep("      register        reset    expected after init    actual");
  ptLogf("      ModeReg           0x3F     0x%02X                   0x%02X",
         PT_REG_MODE_AFTER_INIT, modeAfter);
  ptLogf("      RFCfgReg          0x48     0x%02X                   0x%02X",
         PT_REG_RFCFG_AFTER_INIT, rfcfgAfter);

  bool writesLand = (modeAfter == PT_REG_MODE_AFTER_INIT) &&
                    (rfcfgAfter == PT_REG_RFCFG_AFTER_INIT);
  if (writesLand) {
    recordResult("B1b-writes", PT_PASS, "PCD_Init writes read back correctly");
    logStep("      -> Both changed from their reset value to exactly what");
    logStep("         PCD_Init writes. MOSI and the write path are good.");
  } else if (modeAfter == 0x3F && rfcfgAfter == 0x48) {
    recordResult("B1b-writes", PT_FAIL, "Registers still at reset values");
    logStep("      -> Both are still at their RESET values, so reads work but");
    logStep("         nothing was written. Suspect MOSI: it may be swapped with");
    logStep("         MISO, or not making contact. Reads would still succeed");
    logStep("         because MISO is fine.");
  } else {
    recordResult("B1b-writes", PT_FAIL, "Unexpected post-init register values");
    logStep("      -> The values are neither the reset defaults nor what");
    logStep("         PCD_Init writes. Something is corrupt on the bus. Check");
    logStep("         jumper seating and routing, then re-run.");
  }

  /* B2: the library's own self-test drives the chip's internal test bus and
   * compares against reference firmware images.
   *
   * A failure here is only a hard failure on a genuine NXP part. Clones report
   * version 0x88 and their internal test values do not match the NXP reference
   * tables, so they fail this check while still reading cards perfectly well.
   * Treating that as fatal would send someone debugging a working reader. */
  bool selfTestOk = s_rfid.PCD_PerformSelfTest();
  ptLogf("B2: PCD_PerformSelfTest() = %s", selfTestOk ? "true" : "false");
  if (selfTestOk) {
    recordResult("B2-selftest", PT_PASS, "Chip internal self-test passed");
  } else if (version == 0x88) {
    recordResult("B2-selftest", PT_VISUAL,
                 "Clone chip: self-test mismatch is expected, not a fault");
  } else {
    recordResult("B2-selftest", PT_FAIL,
                 "Self-test failed on a genuine part - suspect power or SPI");
  }

  /* Re-init after the self-test: it leaves the chip's test registers modified,
   * and continuing without a reset is a classic source of "reader worked once
   * then never again" reports. */
  s_rfid.PCD_Init();

  /* B3: first UID read. Timed, because a read that takes seconds points at SPI
   * retries or antenna trouble even when it eventually succeeds. */
  logStep("");
  logStep("B3: PLACE A CARD on the reader now (30s timeout)");
  uint32_t t0 = millis();
  bool gotCard = false;
  while (millis() - t0 < 30000UL) {
    if (s_rfid.PICC_IsNewCardPresent() && s_rfid.PICC_ReadCardSerial()) {
      gotCard = true;
      break;
    }
    delay(40);
  }
  uint32_t readMs = millis() - t0;

  if (!gotCard) {
    recordResult("B3-uid", PT_FAIL, "No card detected in 30s");
    logStep("    CHECK:");
    logStep("      1. The card is 13.56MHz ISO14443A. A 125kHz fob will NEVER");
    logStep("         be seen by an RC522 - test it with an Android phone's NFC.");
    logStep("      2. The card is flat against the antenna, not hovering.");
    logStep("      3. B1 passed, so SPI is fine: this is an antenna or card issue.");
    s_rfid.PICC_HaltA();
    s_rfid.PCD_StopCrypto1();
    s_rfidUsable = false;
    recordResult("B4-stable", PT_SKIP, "Skipped: no card read");
    recordResult("B5-removal", PT_SKIP, "Skipped: no card read");
    return;
  }

  char uidStr[48];
  rfidFormatUid(uidStr, sizeof(uidStr));
  MFRC522::PICC_Type type = MFRC522::PICC_GetType(s_rfid.uid.sak);
  ptLogf("    UID (%u bytes): %s", (unsigned)s_rfid.uid.size, uidStr);
  ptLogf("    SAK: 0x%02X -> %s", s_rfid.uid.sak, ptPiccTypeName(type));
  logFloat("    Time to first read", (float)readMs, 1, " ms");

  /* A 4-byte UID is what the production design assumes. 7- and 10-byte UIDs are
   * legal ISO14443A but would need a wider identity field, so flag it now rather
   * than discovering it in Stage 2. */
  bool uidShapeOk = (s_rfid.uid.size == 4);
  s_rfid.PICC_HaltA();
  s_rfid.PCD_StopCrypto1();

  if (uidShapeOk) {
    recordResult("B3-uid", PT_PASS, uidStr);
  } else {
    recordResult("B3-uid", PT_VISUAL,
                 "Read OK but UID is not 4 bytes - report this");
  }
  s_rfidUsable = true;

  /* B4: read stability. Five consecutive reads must return the identical UID.
   * Intermittent corruption here is the symptom that a marginal SPI clock or a
   * noisy antenna produces, and it is invisible in a single successful read. */
  logStep("");
  logStep("B4: STABILITY - leave the card in place, reading 5 times");
  uint8_t goodReads = 0;
  char firstUid[48];
  firstUid[0] = '\0';
  bool allMatch = true;

  for (uint8_t attempt = 0; attempt < 5; ++attempt) {
    /* WUPA is required to re-read a card that is already in HALT state. */
    uint8_t atqa[2];
    uint8_t sz = sizeof(atqa);
    MFRC522::StatusCode sc = s_rfid.PICC_WakeupA(atqa, &sz);
    if (sc != MFRC522::STATUS_OK && sc != MFRC522::STATUS_COLLISION) {
      ptLogf("    read %u: no card (WUPA -> %s)", (unsigned)(attempt + 1),
             ptStatusCodeName(sc));
      allMatch = false;
      delay(200);
      continue;
    }
    if (s_rfid.PICC_ReadCardSerial()) {
      char thisUid[48];
      rfidFormatUid(thisUid, sizeof(thisUid));
      if (firstUid[0] == '\0') {
        snprintf(firstUid, sizeof(firstUid), "%s", thisUid);
      } else if (strcmp(firstUid, thisUid) != 0) {
        allMatch = false;
      }
      ++goodReads;
      ptLogf("    read %u: %s", (unsigned)(attempt + 1), thisUid);
    } else {
      ptLogf("    read %u: SELECT failed", (unsigned)(attempt + 1));
      allMatch = false;
    }
    s_rfid.PICC_HaltA();
    s_rfid.PCD_StopCrypto1();
    delay(200);
  }

  ptLogf("    %u/5 reads succeeded, UID %s", (unsigned)goodReads,
         allMatch ? "consistent" : "INCONSISTENT");
  if (goodReads == 5 && allMatch) {
    recordResult("B4-stable", PT_PASS, "5/5 identical reads");
  } else {
    recordResult("B4-stable", PT_FAIL, "Unstable reads - shorten jumpers, check 3V3");
  }

  /* B5: removal detection. The production sketch must know when a card has been
   * taken away so it can re-arm for the next player, so the mechanism is tested
   * here rather than assumed. */
  logStep("");
  logStep("B5: REMOVAL - take the card AWAY now (10s timeout)");
  bool removed = rfidWaitRemoval(10000UL);
  if (removed) {
    recordResult("B5-removal", PT_PASS, "Removal detected via WUPA timeout");
  } else {
    recordResult("B5-removal", PT_FAIL,
                 "Still sees a card after 10s - is it still on the reader?");
  }

  logStep("");
  logStep("Phase B done. The reader is usable, so later phases will ask you");
  logStep("to confirm results by TAPPING THE CARD.");
}

#endif /* PHASE_B_RFID_H */
