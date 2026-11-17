/*
 * PhishTap Lite — production firmware (Stage 1b)
 *
 * Non-blocking RC522 state machine: tap detection, removal detection, same-card
 * cooldown and fault reporting.
 *
 * THE CALL SEQUENCE HERE IS HARDWARE-PROVEN, NOT DESIGNED FROM THE DATASHEET
 * Every MFRC522 call below is the one that passed Phase B on this exact board
 * with this exact counterfeit module: UID C7 DA 41 04, SAK 0x08, five identical
 * reads, removal detected. hw_selftest/phase_b_rfid.h is the source, and its
 * comments record two lessons that are easy to lose:
 *
 *   1. PICC_IsNewCardPresent() sends REQA and ALREADY treats STATUS_COLLISION as
 *      "a card answered". A collision is a hit, not an error.
 *   2. Never send a second REQA before SELECT. PICC_ReadCardSerial() runs the
 *      anticollision sequence itself, so an extra REQA in front of it corrupts
 *      the exchange.
 *
 * WHY PICC_IsNewCardPresent() DOES NOT MEAN WHAT ITS NAME SAYS
 * Verified in MFRC522.cpp:1925. It resets TxModeReg, RxModeReg and ModWidthReg,
 * sends REQA, and returns true on STATUS_OK or STATUS_COLLISION. It does NOT call
 * PICC_HaltA(). So it is not detecting a "new" card at all — it is detecting a
 * card that is not currently in the ACTIVE state.
 *
 * That is what makes the standard idiom work, and it is worth being precise
 * about, because the whole tap-detection design rests on it. After
 * PICC_ReadCardSerial() the card is ACTIVE, and per ISO 14443-3 an ACTIVE card
 * answers neither REQA nor WUPA. So the next poll's REQA gets no reply and
 * IsNewCardPresent() returns false, even though the same card is still sitting
 * there. The card only becomes visible to REQA again after it leaves the field or
 * is explicitly halted.
 *
 * WHY REMOVAL NEEDS WUPA AND NOT REQA
 * After reading the UID we call PICC_HaltA(), which puts the card in HALT. A
 * HALT card does not answer REQA, so polling with IsNewCardPresent() would report
 * "absent" for a card that is plainly still on the reader. WUPA is the only
 * command a HALT card answers, which makes it the only correct presence probe.
 *
 * After a successful WUPA the card is no longer in HALT, so the probe must call
 * HaltA again to restore the state it found. Without that the second probe would
 * be testing a card in a different state than the first, and removal detection
 * would work once and then stop working.
 *
 * WHY THIS IS NON-BLOCKING
 * The core's loop is `for(;;) { loop(); __loopHook(); }`. Any delay() here stalls
 * the matrix, the buzzer, the countdown and the bridge handlers together. So the
 * state machine advances a little per call and returns immediately, and the only
 * blocking code is the one-time hardware reset in ptRfidBegin(), which runs from
 * setup() before anything else exists.
 *
 * CLONE MODULES ARE NOT FAULTS
 * This board's reader reports VersionReg 0x82, a counterfeit die. Only 0x00 and
 * 0xFF mean "nothing is answering". Every other value, however undocumented, is a
 * working reader as far as this firmware is concerned. Treating an unknown
 * version as a fault is exactly the mistake that produced four false failures
 * during bring-up and nearly blocked the project on hardware that works.
 */

#ifndef PT_RFID_H
#define PT_RFID_H

#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>

#include "pt_pins.h"
#include "pt_config.h"

/* One instance, file-scope static. Every module here is a header included once
 * from the .ino, so there is a single translation unit and a single instance. */
static MFRC522 s_rfid(PT_RC522_SS, PT_RC522_RST);

typedef enum : uint8_t {
  PT_RFID_ST_UNINIT = 0,
  PT_RFID_ST_SEEK,           /* polling for a card to arrive            */
  PT_RFID_ST_PRESENT_WATCH,  /* a card was read; watching for it to go  */
  PT_RFID_ST_COOLDOWN,       /* card left; suppressing the same UID     */
  PT_RFID_ST_FAULT           /* the reader stopped responding           */
} PtRfidState;

/* Bitmask returned by ptRfidTick, so one call can report a removal and a fault
 * on the same iteration. */
#define PT_RFID_EV_NONE    0u
#define PT_RFID_EV_TAP     1u
#define PT_RFID_EV_REMOVED 2u
#define PT_RFID_EV_FAULT   4u

static struct {
  PtRfidState state;
  uint8_t     version;
  bool        versionKnown;    /* true only for a documented NXP or FM17522 die */
  uint32_t    lastPollMs;
  uint32_t    lastProbeMs;
  uint32_t    cooldownUntilMs;
  uint8_t     readFails;       /* consecutive SELECT failures with a card present */
  /* The UID that the cooldown is suppressing, and when it was last seen. */
  uint8_t     lastUid[10];
  uint8_t     lastUidLen;
  uint32_t    lastUidMs;
  /* The most recent tap, exposed to the caller after PT_RFID_EV_TAP. */
  uint8_t     uid[10];
  uint8_t     uidLen;
  uint8_t     sak;
  char        uidHex[PT_UID_HEX_MAX + 1];
} s_rs;

inline const char* ptRfidStateName(PtRfidState s) {
  switch (s) {
    case PT_RFID_ST_UNINIT:       return "uninit";
    case PT_RFID_ST_SEEK:         return "seek";
    case PT_RFID_ST_PRESENT_WATCH: return "present_watch";
    case PT_RFID_ST_COOLDOWN:     return "cooldown";
    case PT_RFID_ST_FAULT:        return "fault";
    default:                      return "?";
  }
}

/* Only 0x00 and 0xFF mean "nothing answering on the bus". Documented NXP
 * versions are 0x90/0x91/0x92 and 0x88 is a Fudan FM17522; the library has a
 * self-test reference table for exactly those four. Everything else is a
 * counterfeit die, which is common, works, and must not be treated as a fault.
 * See docs/HARDWARE.md and miguelbalboa/rfid issue #428. */
inline bool ptRfidVersionAlive(uint8_t v) {
  return (v != 0x00 && v != 0xFF);
}

inline bool ptRfidVersionDocumented(uint8_t v) {
  return (v == 0x88 || v == 0x90 || v == 0x91 || v == 0x92);
}

/* ── Initialisation ───────────────────────────────────────────────────────
 * BLOCKING, AND ONLY CALLED FROM setup().
 *
 * The delays below are the datasheet's oscillator start-up requirement and
 * cannot be made non-blocking without splitting init across many loop()
 * iterations for no benefit, since nothing else can run until the reader exists.
 */
inline bool ptRfidBegin(void) {
  memset(&s_rs, 0, sizeof(s_rs));
  s_rs.state = PT_RFID_ST_UNINIT;

  /* Bring the bus up the way the official UNO Q SPI example does: SS is a plain
   * output held HIGH (deselected) BEFORE SPI.begin() claims the pins. The
   * board's devicetree sets st,gpio-control on SPI2, meaning chip select is a
   * GPIO rather than a peripheral function, so driving it manually is the
   * intended usage and not a workaround. */
  pinMode(PT_RC522_SS, OUTPUT);
  digitalWrite(PT_RC522_SS, HIGH);
  SPI.begin();

  /* Force a real hardware reset.
   *
   * PCD_Init() only performs a hard reset if digitalRead(RST) reads LOW;
   * otherwise it falls back to a SOFT reset issued over SPI. Many modules leave
   * RST floating or fit a pull-up, so that read returns HIGH, the hardware reset
   * is skipped, and the fallback travels over the very bus we are trying to bring
   * up. If SPI is not talking yet the soft reset does nothing, the chip stays
   * undefined, and every register reads back 0x00 — which is precisely the
   * symptom that cost hours during bring-up before the real cause (unsoldered
   * module headers) was found.
   *
   * Driving the pin ourselves guarantees a genuine reset regardless of what it
   * reads back. The 60 ms wait covers the datasheet's oscillator start-up time
   * (crystal start-up plus 37.74 us) with margin. */
  pinMode(PT_RC522_RST, OUTPUT);
  digitalWrite(PT_RC522_RST, LOW);
  delay(10);
  digitalWrite(PT_RC522_RST, HIGH);
  delay(60);

  s_rfid.PCD_Init();
  s_rs.version        = s_rfid.PCD_ReadRegister(MFRC522::VersionReg);
  s_rs.versionKnown   = ptRfidVersionDocumented(s_rs.version);

  if (!ptRfidVersionAlive(s_rs.version)) {
    s_rs.state = PT_RFID_ST_FAULT;
    return false;
  }

  /* PCD_Init already turns the antenna on, but calling it explicitly is cheap
   * and guards against a clone whose init path differs. PCD_AntennaOn is
   * idempotent: it only sets the TxControlReg bits. */
  s_rfid.PCD_AntennaOn();

  s_rs.state       = PT_RFID_ST_SEEK;
  s_rs.lastPollMs  = millis();
  s_rs.lastProbeMs = s_rs.lastPollMs;
  return true;
}

inline bool ptRfidOk(void) {
  return (s_rs.state != PT_RFID_ST_FAULT) && (s_rs.state != PT_RFID_ST_UNINIT);
}

inline PtRfidState ptRfidGetState(void) { return s_rs.state; }
inline uint8_t ptRfidGetVersion(void) { return s_rs.version; }
inline bool ptRfidVersionIsDocumented(void) { return s_rs.versionKnown; }

/* The UID from the most recent tap, as uppercase hex with no separators. Valid
 * only after ptRfidTick has returned PT_RFID_EV_TAP. */
inline const char* ptRfidGetUidHex(void) { return s_rs.uidHex; }
inline uint8_t ptRfidGetUidLen(void) { return s_rs.uidLen; }
inline uint8_t ptRfidGetSak(void) { return s_rs.sak; }

/* Encode the reader's uid member into s_rs.uidHex. Bounded by the destination
 * field, so a card reporting an unexpected UID length truncates rather than
 * overruns. */
inline void ptRfidEncodeUid(void) {
  static const char hexd[] = "0123456789ABCDEF";
  size_t pos = 0;
  for (uint8_t i = 0; i < s_rfid.uid.size && pos + 2 < sizeof(s_rs.uidHex); ++i) {
    s_rs.uidHex[pos++] = hexd[(s_rfid.uid.uidByte[i] >> 4) & 0x0F];
    s_rs.uidHex[pos++] = hexd[s_rfid.uid.uidByte[i] & 0x0F];
  }
  s_rs.uidHex[pos] = '\0';
}

inline bool ptRfidSameUidAsLast(void) {
  if (s_rfid.uid.size != s_rs.lastUidLen) return false;
  for (uint8_t i = 0; i < s_rs.lastUidLen; ++i) {
    if (s_rfid.uid.uidByte[i] != s_rs.lastUid[i]) return false;
  }
  return true;
}

inline void ptRfidRememberUid(uint32_t now) {
  s_rs.lastUidLen = (s_rfid.uid.size > 10) ? 10 : s_rfid.uid.size;
  for (uint8_t i = 0; i < s_rs.lastUidLen; ++i) s_rs.lastUid[i] = s_rfid.uid.uidByte[i];
  s_rs.lastUidMs = now;
}

/* Attempt a recovery from FAULT by re-initialising. Called rarely, from the app
 * layer, not from the tick: a reader that has genuinely failed should not be
 * hammered every loop iteration, and an endless re-init loop would starve
 * everything else. Returns true if the reader came back. */
inline bool ptRfidTryRecover(void) {
  if (s_rs.state != PT_RFID_ST_FAULT) return true;
  s_rs.readFails = 0;
  bool ok = ptRfidBegin();
  return ok;
}

/* ── The state machine ────────────────────────────────────────────────────
 * Called once per loop() iteration with the same `now` every other engine
 * received, so all of them agree about the current time.
 */
inline uint8_t ptRfidTick(uint32_t now) {
  if (s_rs.state == PT_RFID_ST_FAULT || s_rs.state == PT_RFID_ST_UNINIT) {
    return PT_RFID_EV_NONE;
  }

  /* Leaving the cooldown window is a pure time transition, checked first so a
   * card arriving on the same iteration is handled by SEEK's normal path. */
  if (s_rs.state == PT_RFID_ST_COOLDOWN &&
      (int32_t)(now - s_rs.cooldownUntilMs) >= 0) {
    s_rs.state = PT_RFID_ST_SEEK;
  }

  /* ── PRESENT_WATCH: is the card we already read still there? ─────────── */
  if (s_rs.state == PT_RFID_ST_PRESENT_WATCH) {
    if ((uint32_t)(now - s_rs.lastProbeMs) < PT_RFID_PRESENT_PROBE_MS) {
      return PT_RFID_EV_NONE;
    }
    s_rs.lastProbeMs = now;

    byte atqa[2];
    byte size = sizeof(atqa);
    MFRC522::StatusCode sc = s_rfid.PICC_WakeupA(atqa, &size);

    if (sc == MFRC522::STATUS_OK || sc == MFRC522::STATUS_COLLISION) {
      /* Still present. Restore HALT so the next probe tests the same state as
       * this one did; without this the probe works once and then misreports. */
      s_rfid.PICC_HaltA();
      return PT_RFID_EV_NONE;
    }

    /* No answer: the card is gone. Start suppressing this UID so that putting
     * it straight back down does not create a second session. */
    s_rs.state           = PT_RFID_ST_COOLDOWN;
    s_rs.cooldownUntilMs = now + PT_CARD_COOLDOWN_MS;
    s_rs.lastPollMs      = now;
    return PT_RFID_EV_REMOVED;
  }

  /* ── SEEK and COOLDOWN both poll for a card ────────────────────────────
   * They differ only in whether a matching UID is suppressed, so they share one
   * code path. COOLDOWN still polls, because a DIFFERENT student tapping during
   * the window must be accepted immediately; only the same card is ignored. */
  if ((uint32_t)(now - s_rs.lastPollMs) < PT_RFID_POLL_MS) {
    return PT_RFID_EV_NONE;
  }
  s_rs.lastPollMs = now;

  if (!s_rfid.PICC_IsNewCardPresent()) {
    /* No card answered REQA. This is the NORMAL idle case and must not count as
     * a read failure, or an empty reader would declare a fault within a fraction
     * of a second. Only a card that answers REQA and then fails SELECT is a
     * genuine read fault. */
    s_rs.readFails = 0;
    return PT_RFID_EV_NONE;
  }

  if (!s_rfid.PICC_ReadCardSerial()) {
    /* A card answered REQA but anticollision or SELECT failed. That is a real
     * communication problem: a marginal antenna, a card at the edge of the field,
     * or a reader that is starting to misbehave. Count it, and clean up so the
     * card is not left half-selected. */
    s_rfid.PICC_HaltA();
    s_rfid.PCD_StopCrypto1();
    s_rs.readFails++;
    if (s_rs.readFails >= PT_RFID_FAULT_THRESHOLD) {
      s_rs.state = PT_RFID_ST_FAULT;
      return PT_RFID_EV_FAULT;
    }
    return PT_RFID_EV_NONE;
  }

  /* A card was read successfully. */
  s_rs.readFails = 0;
  s_rs.uidLen    = (s_rfid.uid.size > 10) ? 10 : s_rfid.uid.size;
  for (uint8_t i = 0; i < s_rs.uidLen; ++i) s_rs.uid[i] = s_rfid.uid.uidByte[i];
  s_rs.sak = s_rfid.uid.sak;
  ptRfidEncodeUid();

  /* Halt and stop any crypto before anything else. Leaving the card ACTIVE would
   * make it invisible to the next REQA, and leaving crypto running after a
   * previous authenticated session is how a reader ends up refusing to talk. */
  s_rfid.PICC_HaltA();
  s_rfid.PCD_StopCrypto1();

  /* Same card, still inside its cooldown: suppress the event. This is what stops
   * a student who sets the card down, picks it up and sets it down again from
   * creating three sessions. */
  bool sameCard = ptRfidSameUidAsLast();
  bool inWindow = (s_rs.state == PT_RFID_ST_COOLDOWN) ||
                  ((s_rs.lastUidLen != 0) &&
                   ((uint32_t)(now - s_rs.lastUidMs) < PT_CARD_COOLDOWN_MS));
  if (sameCard && inWindow) {
    s_rs.state           = PT_RFID_ST_COOLDOWN;
    s_rs.cooldownUntilMs = s_rs.lastUidMs + PT_CARD_COOLDOWN_MS;
    return PT_RFID_EV_NONE;
  }

  ptRfidRememberUid(now);
  s_rs.state       = PT_RFID_ST_PRESENT_WATCH;
  s_rs.lastProbeMs = now;
  return PT_RFID_EV_TAP;
}

#endif /* PT_RFID_H */
