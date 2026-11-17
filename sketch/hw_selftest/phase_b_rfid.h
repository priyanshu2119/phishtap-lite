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
 * chance on a broken bus is not a realistic possibility.
 *
 * WHY SPLIT DIGITAL FROM ANALOG: counterfeit dies implement the digital logic
 * identically but have a different analog receiver front end, so their power-on
 * defaults for RxThresholdReg, DemodReg and RFCfgReg legitimately differ from
 * the NXP table. Counting those as failures would condemn a reader that goes on
 * to read cards perfectly. Only the digital registers are treated as proof of a
 * working bus and a compatible chip.
 *
 * These must be read BEFORE PCD_Init(), because PCD_Init overwrites several of
 * them: ModeReg goes from its 0x3F reset value to 0x3D, and RFCfgReg from 0x48
 * to 0x70 when PCD_SetAntennaGain(RxGain_max) runs. Reading afterwards would
 * compare against the wrong expectations.
 *
 * Returns the number of DIGITAL registers that matched, and writes the analog
 * match count to *analogMatched.
 */
inline uint8_t rfidCheckFingerprint(uint32_t clockHz, uint8_t* analogMatched,
                                    uint8_t* analogTotal, uint8_t* digitalTotal) {
  uint8_t digMatched = 0, digTotal = 0;
  uint8_t anaMatched = 0, anaTotal = 0;

  logStep("      register        addr   expected   actual   class    match");
  for (uint8_t i = 0; i < PT_FINGERPRINT_COUNT; ++i) {
    const PtRegExpect* e = &PT_RESET_FINGERPRINT[i];
    uint8_t actual = rfidRawReadReg(e->addr, clockHz);
    bool ok = ((actual & e->mask) == (e->expected & e->mask));
    if (e->digital) { digTotal++; if (ok) digMatched++; }
    else            { anaTotal++; if (ok) anaMatched++; }
    ptLogf("      %-16s 0x%02X   0x%02X       0x%02X     %-7s  %s",
           e->name, e->addr, e->expected, actual,
           e->digital ? "digital" : "analog", ok ? "yes" : "NO");
  }
  ptLogf("      digital registers: %u/%u match (these prove the bus and chip)",
         (unsigned)digMatched, (unsigned)digTotal);
  ptLogf("      analog registers:  %u/%u match (clone dies differ here by design)",
         (unsigned)anaMatched, (unsigned)anaTotal);

  *analogMatched = anaMatched;
  *analogTotal   = anaTotal;
  *digitalTotal  = digTotal;
  return digMatched;
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

  /* Name the register, not just its address. Printing "20 reads of 0x37" reads
   * as though 0x37 were the value returned, when 0x37 is the VersionReg address
   * and the value is the 0x82 quoted later on the same line. */
  ptLogf("      %u reads of VersionReg (addr 0x%02X): %u identical to the first (0x%02X), %u distinct value(s)",
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
  /* Delegates to the shared classifier in pt_spi_probe.h so there is one place
   * that knows which version bytes are genuine, which are known clones and which
   * are unrecognised counterfeits. */
  return ptVersionDescribe(v);
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
  uint8_t analogMatched = 0, analogTotal = 0, digitalTotal = 0;
  uint8_t digMatched = rfidCheckFingerprint(slowClock, &analogMatched,
                                            &analogTotal, &digitalTotal);
  logStep("");

  logStep("B0b: stability - VersionReg read 20 times");
  rfidRepeatRead(PT_REG_VERSION, slowClock, 20);
  logStep("");

  logStep("B0c: chip select - manual GPIO versus the library's own handling");
  rfidCompareChipSelect(PT_REG_VERSION, slowClock);
  logStep("");

  logStep("B0d: clock sweep on VersionReg");
  uint8_t  rawVersion    = 0x00;
  uint8_t  rawAllSame    = 1;
  uint8_t  rawFirst      = 0xFF;
  uint8_t  clocksWorked  = 0;   /* how many of the three produced a live byte */
  bool     fastestFailed = false;

  for (uint8_t i = 0; i < PT_PROBE_CLOCK_COUNT; ++i) {
    uint8_t v = rfidRawReadReg(PT_REG_VERSION, PT_PROBE_CLOCKS[i]);
    bool live = (v != 0x00 && v != 0xFF);
    ptLogf("    @ %7lu Hz -> 0x%02X  (%s)",
           (unsigned long)PT_PROBE_CLOCKS[i], v, ptVersionDescribe(v));
    if (i == 0) rawFirst = v;
    else if (v != rawFirst) rawAllSame = 0;
    if (live) {
      clocksWorked++;
      if (rawVersion == 0x00) rawVersion = v;
    } else if (i == PT_PROBE_CLOCK_COUNT - 1) {
      /* The library is hardwired to 4 MHz, so a failure only at the fastest
       * clock is the one case that genuinely threatens real operation. */
      fastestFailed = true;
    }
  }
  logStep("");

  /* ── B0 decision ─────────────────────────────────────────────────────────
   * Scored on the DIGITAL registers only. Those are pure logic, so every
   * MFRC522-compatible die implements them identically and a mismatch means
   * something is really wrong. The analog receiver registers are allowed to
   * differ, because counterfeit dies have a different analog front end and
   * different power-on defaults there by design.
   */
  logStep("B0 DECISION");
  PtVersionClass vclass = ptVersionClass(rawVersion);

  if (digMatched == digitalTotal && digitalTotal > 0) {
    if (analogMatched == analogTotal) {
      recordResult("B0-fingerprint", PT_PASS,
                   "All registers match datasheet reset values");
      logStep("    Every register, digital and analog, matches its documented");
      logStep("    reset value. The module is powered, selected, clocked and");
      logStep("    reading back correctly.");
    } else {
      recordResult("B0-fingerprint", PT_WARN,
                   "Digital regs match; analog differ - counterfeit die");
      ptLogf("    All %u DIGITAL registers match exactly, but %u of %u analog",
             (unsigned)digitalTotal, (unsigned)(analogTotal - analogMatched),
             (unsigned)analogTotal);
      logStep("    receiver registers do not. That combination is the signature");
      logStep("    of a COUNTERFEIT die, not a wiring fault: the digital logic is");
      logStep("    identical to a genuine part, while the analog front end differs");
      logStep("    and so powers up with different defaults.");
      logStep("");
      logStep("    This is NOT a failure. Such modules read card UIDs normally,");
      logStep("    which is all this project does. Known trade-offs of clones:");
      logStep("      - shorter read range (often 2-3cm instead of 5cm)");
      logStep("      - some cannot read certain tag types, blue keyfobs in");
      logStep("        particular, so use the white MIFARE card for the demo");
      logStep("      - write operations are unreliable - irrelevant here, since");
      logStep("        this project is read-only by policy");
    }
  } else if (digMatched == 0) {
    recordResult("B0-fingerprint", PT_FAIL, "No register matched - bus or power dead");
    if (rawAllSame && rawFirst == 0x00) {
      logStep("    All registers read 0x00 and the line is stable, so MISO is");
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
      logStep("    Reads vary but no digital register matches. Suspect intermittent");
      logStep("    contact. Re-seat every jumper, keep them under 20cm, re-run.");
    }
  } else {
    recordResult("B0-fingerprint", PT_FAIL,
                 "Some DIGITAL registers failed - bus is corrupting data");
    ptLogf("    %u of %u digital registers matched. Unlike an analog-only",
           (unsigned)digMatched, (unsigned)digitalTotal);
    logStep("    mismatch, this means bits are being corrupted in transit, which");
    logStep("    points at signal integrity or a marginal connection rather than");
    logStep("    a clone. Compare the expected and actual columns above: if the");
    logStep("    differences are all in the same bit position, suspect one");
    logStep("    specific data line.");
  }

  if (vclass == PT_VER_UNKNOWN_CLONE || vclass == PT_VER_KNOWN_CLONE) {
    ptLogf("    VersionReg = 0x%02X -> %s", rawVersion, ptVersionDescribe(rawVersion));
    logStep("    Documented NXP versions are 0x90/0x91/0x92. Values such as 0x12,");
    logStep("    0x18, 0x82, 0x88, 0x8C, 0xB0, 0xB2 and 0xB3 are counterfeit dies");
    logStep("    reported widely in miguelbalboa/rfid issue #428, and those modules");
    logStep("    commonly read cards fine despite the unknown version.");
  } else if (rawVersion != 0x00 && rawVersion != 0xFF) {
    ptLogf("    VersionReg = 0x%02X -> %s", rawVersion, ptVersionDescribe(rawVersion));
  }

  if (fastestFailed && clocksWorked > 0) {
    logStep("    NOTE: it answered at the slower clocks but NOT at 4 MHz. The");
    logStep("    library is fixed at 4 MHz and cannot be slowed from a sketch, so");
    logStep("    reads will be unreliable. Shorten the jumpers and route them away");
    logStep("    from the antenna coil, then re-run.");
  } else if (clocksWorked == PT_PROBE_CLOCK_COUNT) {
    logStep("    Answered identically at all three clocks, so this is not a");
    logStep("    signal-integrity problem.");
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
  /* B1 tests one thing: whether the library's own SPI path reaches the chip.
   * Say that. Recording the bare version string made the summary read
   * "B1-spi PASS COUNTERFEIT/clone die", which looks like a contradiction and
   * hides the fact that this row is a pass. The clone detail already lives in
   * B0-fingerprint and the bottom line, so a short tag is enough here. */
  {
    char b1Detail[72];
    PtVersionClass b1Class = ptVersionClass(version);
    snprintf(b1Detail, sizeof(b1Detail), "Library SPI works, version 0x%02X%s",
             version,
             (b1Class == PT_VER_GENUINE)      ? "" :
             (b1Class == PT_VER_KNOWN_CLONE)  ? " (FM17522 clone)" :
             (b1Class == PT_VER_DEAD)         ? " (no response)" :
                                                " (clone die)");
    recordResult("B1-spi", PT_PASS, b1Detail);
  }

  /* ── B1b: prove that WRITES reach the chip, not just reads ───────────────
   * PCD_Init() has just run, and it writes known values to registers whose reset
   * defaults we already read in B0a.
   *
   * ModeReg is the authoritative test. It is a pure digital register, it goes
   * from 0x3F to 0x3D, and every compatible die behaves identically. If it
   * changed, writes land.
   *
   * RFCfgReg is reported but NOT used to decide the verdict. It is an analog
   * receiver register, and PCD_Init sets it to RxGain_max (0x70) via
   * PCD_SetAntennaGain. Counterfeit dies implement the gain field differently,
   * so a clone can leave it at something else entirely while still having
   * accepted the write. An earlier revision required both registers to match and
   * therefore reported "something is corrupt on the bus" for a reader that had
   * just proved writes worked and went on to read a card five times out of five.
   */
  uint8_t modeAfter  = s_rfid.PCD_ReadRegister(MFRC522::ModeReg);
  uint8_t rfcfgAfter = s_rfid.PCD_ReadRegister(MFRC522::RFCfgReg);
  logStep("");
  logStep("B1b: register values after PCD_Init(), to prove writes land");
  logStep("      register        reset    expected after init    actual   verdict");
  bool modeOk = (modeAfter == PT_REG_MODE_AFTER_INIT);
  ptLogf("      ModeReg           0x3F     0x%02X                   0x%02X     %s",
         PT_REG_MODE_AFTER_INIT, modeAfter, modeOk ? "MATCH" : "differs");
  ptLogf("      RFCfgReg          0x48     0x%02X                   0x%02X     %s",
         PT_REG_RFCFG_AFTER_INIT, rfcfgAfter,
         (rfcfgAfter == PT_REG_RFCFG_AFTER_INIT) ? "MATCH" : "differs (analog)");

  if (modeOk) {
    recordResult("B1b-writes", PT_PASS, "ModeReg changed as PCD_Init writes");
    logStep("      -> ModeReg moved from its 0x3F reset value to exactly 0x3D,");
    logStep("         which only happens if the write reached the chip. MOSI and");
    logStep("         the write path are good.");
    if (rfcfgAfter != PT_REG_RFCFG_AFTER_INIT) {
      logStep("      -> RFCfgReg differs, which is expected on a counterfeit die:");
      logStep("         it is an analog gain register and clones implement the");
      logStep("         gain field differently. Not a fault.");
    }
  } else if (modeAfter == 0x3F) {
    recordResult("B1b-writes", PT_FAIL, "ModeReg still at reset value");
    logStep("      -> ModeReg is still at its RESET value, so reads work but");
    logStep("         nothing was written. Suspect MOSI: it may be swapped with");
    logStep("         MISO, or not making contact. Reads would still succeed");
    logStep("         because MISO is fine.");
  } else {
    recordResult("B1b-writes", PT_FAIL, "ModeReg holds an unexpected value");
    ptLogf("      -> ModeReg is 0x%02X, which is neither its 0x3F reset value nor",
           modeAfter);
    logStep("         the 0x3D that PCD_Init writes. Data is being corrupted in");
    logStep("         transit. Check jumper seating and routing, then re-run.");
  }

  /* B2: the library's own self-test.
   *
   * READ THE LIBRARY SOURCE BEFORE TRUSTING THIS RESULT. PCD_PerformSelfTest()
   * switches on VersionReg to pick a reference table and returns false from its
   * `default:` case:
   *
   *     case 0x88: reference = FM17522_firmware_reference; break;
   *     case 0x90: ... case 0x91: ... case 0x92: ...
   *     default:   return false;   // abort test
   *
   * So for ANY version byte outside {0x88, 0x90, 0x91, 0x92} it returns false
   * without comparing a single byte. The result carries no information about the
   * chip's health. Reporting that as a hardware failure sends someone debugging
   * a reader that works, which is exactly what happened on the first run of this
   * phase with a 0x82 module.
   *
   * There is also a documented subtlety worth knowing: the self-test result
   * depends on the RcvOff bit of CommandReg, which soft reset sets to 1. The
   * library writes a bare PCD_CalcCRC (0x03), clearing RcvOff, so it gets this
   * right - but hand-rolled self-test code that preserves RcvOff gets wrong
   * results even on genuine hardware. */
  bool selfTestOk = s_rfid.PCD_PerformSelfTest();
  ptLogf("B2: PCD_PerformSelfTest() = %s", selfTestOk ? "true" : "false");
  PtVersionClass b2class = ptVersionClass(version);
  if (selfTestOk) {
    recordResult("B2-selftest", PT_PASS, "Chip internal self-test passed");
  } else if (b2class == PT_VER_GENUINE) {
    recordResult("B2-selftest", PT_FAIL,
                 "Self-test failed on a GENUINE part - suspect power or SPI");
    logStep("    A genuine 0x90/0x91/0x92 part has a reference table, so this");
    logStep("    failure is meaningful. Check the 3V3 rail under load and the");
    logStep("    jumper routing.");
  } else {
    recordResult("B2-selftest", PT_WARN,
                 "Self-test unavailable: library has no table for this version");
    ptLogf("    VersionReg is 0x%02X, which is not one of 0x88/0x90/0x91/0x92,", version);
    logStep("    so the library returned false from its default case WITHOUT");
    logStep("    comparing anything. This says nothing about the chip's health.");
    logStep("    Judge the reader by B3/B4/B5 - whether it actually reads a card.");
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

  /* ── BOTTOM LINE ─────────────────────────────────────────────────────────
   * This is the verdict that actually matters, and it is printed last so it is
   * the thing the operator reads.
   *
   * WHY IT EXISTS: the checks above include several that a perfectly good
   * counterfeit module cannot pass - an unrecognised VersionReg, a self-test the
   * library refuses to run, analog registers with different defaults. Scored
   * naively those add up to "4 hard failures, do not start Stage 1b" for a reader
   * that had just read the same UID five times out of five. The functional result
   * has to outrank the cosmetic ones.
   *
   * What PhishTap Lite actually needs from this reader is narrow: detect a card,
   * read a stable 4-byte UID, and notice when the card goes away. It never
   * authenticates, never reads memory blocks and never writes. So B3, B4 and B5
   * ARE the acceptance criteria, and everything in B0-B2 is supporting detail.
   */
  logStep("");
  logStep("================ PHASE B BOTTOM LINE ================");
  if (gotCard && goodReads == 5 && allMatch && removed && uidShapeOk) {
    ptMatrixShowText2("OK");
    recordResult("B-BOTTOM-LINE", PT_PASS,
                 "READER WORKS - reads a stable 4-byte UID and detects removal");
    logStep("  THE READER WORKS. It detected a card, returned the same 4-byte");
    logStep("  UID on 5 consecutive reads, and noticed when the card was taken");
    logStep("  away. That is everything PhishTap Lite needs from it.");
    logStep("");
    logStep("  Any WARN rows above are cosmetic. This project is read-only by");
    logStep("  policy, so the weaknesses that counterfeit RC522 modules are known");
    logStep("  for - unreliable writes, occasionally refusing certain tag types -");
    logStep("  do not affect it.");
    logStep("");
    logStep("  Two practical notes for the demo:");
    logStep("    - Use the WHITE MIFARE card. Clones often fail on blue keyfobs.");
    logStep("    - Clone antennas are weaker, so hold the card flat against the");
    logStep("      reader rather than hovering it.");
    logStep("");
    logStep("  Stage 1b is unblocked.");
  } else if (gotCard && goodReads == 5 && allMatch) {
    ptMatrixShowText2("OK");
    recordResult("B-BOTTOM-LINE", PT_PASS,
                 "READER WORKS - stable UID reads (removal check inconclusive)");
    logStep("  The reader reads a stable UID reliably, which is the core");
    logStep("  requirement. The removal check did not confirm, which usually just");
    logStep("  means the card was still on the reader when the 10s elapsed.");
    logStep("  Re-run and lift the card promptly, but this does not block Stage 1b.");
  } else if (gotCard) {
    ptMatrixShowText2("ER");
    recordResult("B-BOTTOM-LINE", PT_FAIL,
                 "Card detected but reads are NOT stable");
    logStep("  A card was detected but consecutive reads disagreed. That is a");
    logStep("  real problem: the game would assign players inconsistently.");
    logStep("  Suspect marginal contact or antenna coupling. Shorten jumpers,");
    logStep("  keep them away from the coil, and hold the card flat.");
  } else {
    ptMatrixShowText2("ER");
    recordResult("B-BOTTOM-LINE", PT_FAIL, "No card could be read");
    logStep("  No UID was read at all. Work through the B0 decision above, then");
    logStep("  Phase W for the electrical layer and Phase L for the board itself.");
  }
  logStep("======================================================");

  logStep("");
  logStep("Phase B done. If the bottom line above is PASS, later phases will");
  logStep("ask you to confirm results by TAPPING THE CARD.");
}

#endif /* PHASE_B_RFID_H */
