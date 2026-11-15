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
 * sketch changes nothing at all — it only affects code in our own translation
 * unit, and we issue no SPI transactions of our own through the library.
 *
 * The define is kept (harmlessly) so the intent is documented, but the real
 * slow-clock test is performed by rfidRawReadReg() below, which drives the bus
 * directly and can therefore use any clock it likes.
 */
#ifndef MFRC522_SPICLOCK
#define MFRC522_SPICLOCK 1000000u
#endif

#include <MFRC522.h>

#include "pt_pins.h"
#include "pt_log.h"

static MFRC522 s_rfid(PT_RC522_SS, PT_RC522_RST);

/* Set once Phase B has proven the reader works. Later phases use a card tap as
 * the human-confirmation input, and they need to know whether that is available
 * or whether they must fall back to "judge this by eye". */
static bool s_rfidUsable = false;

/* ── Raw SPI access, deliberately independent of the MFRC522 library ────────
 * When a reader does not respond there are two very different explanations:
 * the SPI bus is not working, or the bus works but the library/chip is in a bad
 * state. Going through the library cannot distinguish them, because a failure
 * looks identical either way. These two helpers talk to the chip directly so
 * the bus can be proven or ruled out on its own terms.
 */

/*
 * Force a hardware reset of the RC522.
 *
 * WHY THIS IS NOT OPTIONAL: MFRC522::PCD_Init() only performs a hard reset if it
 * first reads the RST pin as LOW:
 *
 *     pinMode(_resetPowerDownPin, INPUT);
 *     if (digitalRead(_resetPowerDownPin) == LOW) { ...hard reset...; }
 *     if (!hardReset) { PCD_Reset(); }        // soft reset, over SPI
 *
 * Many RC522 modules leave RST floating or fit a pull-up, so that read returns
 * HIGH, the hard reset is skipped, and the code falls back to PCD_Reset() — a
 * SOFT reset issued over SPI. If SPI is not yet talking, or the chip is in an
 * undefined state, the soft reset achieves nothing and every register then reads
 * back as 0x00. This is the single most common RC522 bring-up failure.
 *
 * Driving the pin LOW then HIGH ourselves guarantees a real hardware reset
 * regardless of what the pin happens to read, and costs 60 ms once at startup.
 * The 60 ms covers the datasheet's oscillator start-up time (crystal + 37.74 us)
 * with margin.
 */
inline void rfidHardReset(void) {
  pinMode(PT_RC522_RST, OUTPUT);
  digitalWrite(PT_RC522_RST, LOW);
  delay(10);                       /* hold reset; datasheet minimum is ~100 ns */
  digitalWrite(PT_RC522_RST, HIGH);
  delay(60);                       /* oscillator start-up, with margin */
}

/*
 * Read one register over raw SPI at a chosen clock rate.
 *
 * `rawAddr` is the UNshifted datasheet register address (VersionReg is 0x37).
 * Per datasheet section 8.1.2.3 the SPI address byte is (addr << 1) & 0x7E with
 * the MSB set for a read, which for 0x37 yields 0xEE — byte-for-byte what the
 * library sends, so the two paths are directly comparable.
 */
inline uint8_t rfidRawReadReg(uint8_t rawAddr, uint32_t clockHz) {
  uint8_t addrByte = (uint8_t)(0x80 | ((rawAddr << 1) & 0x7E));
  SPI.beginTransaction(SPISettings(clockHz, MSBFIRST, SPI_MODE0));
  digitalWrite(PT_RC522_SS, LOW);
  SPI.transfer(addrByte);
  uint8_t value = SPI.transfer(0x00);
  digitalWrite(PT_RC522_SS, HIGH);
  SPI.endTransaction();
  return value;
}

#define PT_RC522_REG_VERSION 0x37   /* datasheet VersionReg, unshifted */

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

  /* ── B0: raw SPI probe, before the library is involved at all ──────────────
   * Reading VersionReg directly over SPI answers the one question that matters
   * most and that the library cannot answer: is the bus alive? It also sweeps
   * the clock, because the library is hard-wired to 4 MHz and a marginal wiring
   * job often works at 100 kHz while failing at 4 MHz.
   *
   * A hardware reset is forced first. See rfidHardReset() for why PCD_Init's own
   * reset cannot be relied upon.
   */
  rfidHardReset();

  logStep("B0: raw SPI probe of VersionReg (0x37), bypassing the library");
  static const uint32_t probeClocks[] = { 100000u, 1000000u, 4000000u };
  const uint8_t probeCount =
      (uint8_t)(sizeof(probeClocks) / sizeof(probeClocks[0]));
  uint8_t  rawVersion   = 0x00;
  uint32_t rawBestClock = 0;
  uint8_t  rawAllSame   = 1;
  uint8_t  rawFirst     = 0xFF;

  for (uint8_t i = 0; i < probeCount; ++i) {
    uint8_t v = rfidRawReadReg(PT_RC522_REG_VERSION, probeClocks[i]);
    ptLogf("    @ %7lu Hz -> 0x%02X  (%s)",
           (unsigned long)probeClocks[i], v, ptVersionName(v));
    if (i == 0) rawFirst = v;
    else if (v != rawFirst) rawAllSame = 0;
    /* Remember the slowest clock that produced a plausible version byte. */
    if (rawVersion == 0x00 && v != 0x00 && v != 0xFF) {
      rawVersion   = v;
      rawBestClock = probeClocks[i];
    }
  }

  if (rawVersion != 0x00) {
    ptLogf("    Raw SPI is ALIVE. Version 0x%02X, best at %lu Hz.",
           rawVersion, (unsigned long)rawBestClock);
    if (rawBestClock < 4000000u) {
      logStep("    NOTE: it only answered below 4 MHz. The library is fixed at");
      logStep("    4 MHz, so reads may be unreliable - shorten the jumpers and");
      logStep("    keep them away from the antenna coil.");
    }
    recordResult("B0-rawspi", PT_PASS, "Raw SPI answered");
  } else if (rawAllSame && rawFirst == 0x00) {
    recordResult("B0-rawspi", PT_FAIL, "MISO reads 0x00 at every clock");
    logStep("    The bus is not getting a response. CHECK, in this order:");
    logStep("      1. VCC on 3V3 (NOT 5V) and GND common with the board.");
    logStep("         Measure 3.3V at the module's own pins, not at the board.");
    logStep("      2. MISO is on D12 and MOSI on D11. Swapping these is the");
    logStep("         single most common wiring error, and module silkscreen");
    logStep("         order varies between sellers - read the label, not the");
    logStep("         position.");
    logStep("      3. SDA/SS on D10, SCK on D13, RST on D5.");
    logStep("      4. Header solder joints on the module are actually made.");
    logStep("         Un-soldered headers are extremely common on cheap RC522s.");
    logStep("      5. Jumpers under 20cm and not bundled with the antenna coil.");
  } else if (rawAllSame && rawFirst == 0xFF) {
    recordResult("B0-rawspi", PT_FAIL, "MISO reads 0xFF at every clock");
    logStep("    MISO is stuck high or floating. CHECK:");
    logStep("      1. Is MISO actually connected to D12? A floating input");
    logStep("         reads 0xFF.");
    logStep("      2. Module VCC/GND present - an unpowered module cannot drive");
    logStep("         MISO low.");
    logStep("      3. MOSI/MISO not swapped.");
  } else {
    recordResult("B0-rawspi", PT_FAIL, "Inconsistent reads at every clock");
    logStep("    Values differ between clock rates and none is a valid version.");
    logStep("    Suspect intermittent wiring, a loose dupont connector, or noise.");
    logStep("    Re-seat every jumper and keep them short.");
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
