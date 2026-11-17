/*
 * PhishTap Lite — hw_selftest
 *
 * Raw SPI probing helpers, shared by Phase L (loopback), Phase W (MISO line
 * state) and Phase B (RC522).
 *
 * WHY A SEPARATE LAYER
 *   The MFRC522 library cannot be used to diagnose the SPI bus, because a
 *   failure looks identical whether the bus is dead or the chip is unhappy. These
 *   helpers drive the bus directly so the two can be told apart.
 *
 *   They also get to choose the clock, which the library does not. Every
 *   SPI.beginTransaction in MFRC522.cpp expands MFRC522_SPICLOCK inside the
 *   library's own translation unit, so a sketch-side #define never reaches it and
 *   the library always runs at 4 MHz. Sweeping the clock is only possible from
 *   here.
 *
 * REGISTER RESET VALUES
 *   Taken from the NXP MFRC522 product data sheet, Rev. 3.9, 27 April 2016,
 *   which documents each register as "Table N. <Reg> register (address XXh);
 *   reset value: YYh". These are the values a healthy chip presents immediately
 *   after a hardware reset and BEFORE any configuration write, which is what
 *   makes them a fingerprint.
 *
 *   VersionReg has no reset-value table entry. Section 9.3.4.8 states version 1.0
 *   reads 91h and version 2.0 reads 92h. Clones using the Fudan FM17522 die read
 *   88h and still work normally.
 */

#ifndef PT_SPI_PROBE_H
#define PT_SPI_PROBE_H

#include <Arduino.h>
#include <SPI.h>

#include "pt_pins.h"
#include "pt_log.h"

/* Unshifted datasheet register addresses. The wire format shifts them left by
 * one and masks to 0x7E, then sets the MSB for a read (datasheet 8.1.2.3). */
#define PT_REG_COMMAND      0x01
#define PT_REG_COMIEN       0x02
#define PT_REG_STATUS1      0x07
#define PT_REG_MODE         0x11
#define PT_REG_TXCONTROL    0x14
#define PT_REG_RXTHRESHOLD  0x18
#define PT_REG_DEMOD        0x19
#define PT_REG_RFCFG        0x26
#define PT_REG_GSN          0x27
#define PT_REG_AUTOTEST     0x36
#define PT_REG_VERSION      0x37

/* What the library writes during PCD_Init(). Reading these back afterwards
 * proves that WRITES reach the chip, not just reads. ModeReg goes from its 0x3F
 * reset value to 0x3D, and RFCfgReg from 0x48 to 0x70 because PCD_Init calls
 * PCD_SetAntennaGain(RxGain_max). */
#define PT_REG_MODE_AFTER_INIT      0x3D
#define PT_REG_RFCFG_AFTER_INIT     0x70

/*
 * One row of the post-reset fingerprint.
 *
 * `mask` lets a register be compared loosely where a bit is documented as
 * changing on its own. Status1Reg's low bits reflect the CRC and timer state and
 * can move, so only the documented reset bits are asserted there.
 *
 * `digital` separates two very different kinds of register, and this distinction
 * is what makes the fingerprint usable on real hardware rather than only on a
 * genuine NXP part:
 *
 *   digital = true   Control, interrupt, status and mode registers. These are
 *                    pure logic, so every MFRC522-compatible die implements them
 *                    identically. A mismatch here means the bus or the chip is
 *                    genuinely wrong.
 *
 *   digital = false  Receiver analog front-end configuration. Counterfeit dies
 *                    have a different analog design, so their power-on defaults
 *                    legitimately differ from the NXP table. A mismatch here is
 *                    evidence of a clone, NOT evidence of a fault.
 *
 * Measured on a real module reporting version 0x82: all seven digital registers
 * matched exactly, while RxThresholdReg, DemodReg and RFCfgReg - all three
 * analog - did not. That pattern is a clone signature, and the reader went on to
 * read a card UID five times out of five.
 */
typedef struct {
  const char* name;
  uint8_t     addr;
  uint8_t     expected;
  uint8_t     mask;      /* bits that must match; 0xFF means all of them */
  bool        digital;   /* true = must match on any compatible die */
} PtRegExpect;

static const PtRegExpect PT_RESET_FINGERPRINT[] = {
  { "CommandReg",     PT_REG_COMMAND,     0x20, 0xFF, true  }, /* RcvOff=1, PowerDown=0 */
  { "ComIEnReg",      PT_REG_COMIEN,      0x80, 0xFF, true  },
  { "Status1Reg",     PT_REG_STATUS1,     0x21, 0x21, true  }, /* low bits can move */
  { "ModeReg",        PT_REG_MODE,        0x3F, 0xFF, true  },
  { "TxControlReg",   PT_REG_TXCONTROL,   0x80, 0x83, true  }, /* bit 7 plus Tx1/Tx2RFEn */
  { "GsNReg",         PT_REG_GSN,         0x88, 0xFF, false },
  { "AutoTestReg",    PT_REG_AUTOTEST,    0x40, 0xFF, true  },
  { "RxThresholdReg", PT_REG_RXTHRESHOLD, 0x84, 0xFF, false }, /* analog: clone-variable */
  { "DemodReg",       PT_REG_DEMOD,       0x4D, 0xFF, false }, /* analog: clone-variable */
  { "RFCfgReg",       PT_REG_RFCFG,       0x48, 0xFF, false }, /* analog: clone-variable */
};

#define PT_FINGERPRINT_COUNT \
  (sizeof(PT_RESET_FINGERPRINT) / sizeof(PT_RESET_FINGERPRINT[0]))

/*
 * Classify a VersionReg byte.
 *
 * The NXP datasheet section 9.3.4.8 documents only 91h (v1.0) and 92h (v2.0);
 * the library additionally knows 90h (v0.0) and 88h (Fudan FM17522). Everything
 * else is a counterfeit die. The values listed here were collected from
 * miguelbalboa/rfid issue #428 ("[Collection] Bad boards") and the Arduino
 * forum, where users report 0x12, 0x18, 0x82, 0x8C, 0xB0, 0xB2 and 0xB3 on
 * modules that nevertheless read cards.
 *
 * WHY THIS MATTERS: PCD_PerformSelfTest() switches on the version and returns
 * false from its `default:` case for anything it does not recognise. It aborts
 * BEFORE comparing a single byte, so on a clone the self-test result says
 * nothing at all about the chip's health. Treating that false as a hardware
 * failure sends someone debugging a reader that works.
 */
typedef enum {
  PT_VER_GENUINE = 0,   /* 0x90 / 0x91 / 0x92 - NXP, self-test meaningful */
  PT_VER_KNOWN_CLONE,   /* 0x88 - FM17522, library HAS a reference table   */
  PT_VER_UNKNOWN_CLONE, /* anything else - no reference table exists       */
  PT_VER_DEAD           /* 0x00 or 0xFF - nothing answering on the bus     */
} PtVersionClass;

inline PtVersionClass ptVersionClass(uint8_t v) {
  if (v == 0x00 || v == 0xFF) return PT_VER_DEAD;
  if (v == 0x90 || v == 0x91 || v == 0x92) return PT_VER_GENUINE;
  if (v == 0x88) return PT_VER_KNOWN_CLONE;
  return PT_VER_UNKNOWN_CLONE;
}

inline const char* ptVersionDescribe(uint8_t v) {
  switch (ptVersionClass(v)) {
    case PT_VER_GENUINE:
      return (v == 0x90) ? "genuine MFRC522 v0.0"
                         : ((v == 0x91) ? "genuine MFRC522 v1.0"
                                        : "genuine MFRC522 v2.0");
    case PT_VER_KNOWN_CLONE:
      return "FM17522 clone (Fudan) - library has a reference table";
    case PT_VER_UNKNOWN_CLONE:
      return "COUNTERFEIT/clone die - version not in the NXP datasheet";
    case PT_VER_DEAD:
      return (v == 0x00) ? "NO RESPONSE - MISO stuck low"
                         : "NO RESPONSE - MISO stuck high or floating";
  }
  return "?";
}

/* The three clocks every probe sweeps. 100 kHz is slow enough to survive almost
 * any jumper quality, 4 MHz is what the library is hardwired to, and 1 MHz sits
 * between them. A chip that answers at 100 kHz but not at 4 MHz has a signal
 * integrity problem rather than a wiring fault. */
static const uint32_t PT_PROBE_CLOCKS[] = { 100000u, 1000000u, 4000000u };
#define PT_PROBE_CLOCK_COUNT (sizeof(PT_PROBE_CLOCKS) / sizeof(PT_PROBE_CLOCKS[0]))

/*
 * Force a hardware reset of the RC522 by driving RST low then high.
 *
 * WHY THIS CANNOT BE LEFT TO THE LIBRARY: MFRC522::PCD_Init() only performs a
 * hardware reset if it first reads the RST pin as LOW.
 *
 *     pinMode(_resetPowerDownPin, INPUT);
 *     if (digitalRead(_resetPowerDownPin) == LOW) { ...hard reset...; }
 *     if (!hardReset) { PCD_Reset(); }     // soft reset, issued over SPI
 *
 * Many modules leave RST floating or fit a pull-up, so that read returns HIGH,
 * the hardware reset is skipped, and the fallback is a SOFT reset that travels
 * over the very SPI bus we are trying to prove works. If SPI is not talking yet
 * the soft reset does nothing, the chip stays in an undefined state, and every
 * register reads back as 0x00.
 *
 * Driving the pin ourselves guarantees a real reset regardless of what it reads
 * back. The 60 ms wait covers the datasheet's oscillator start-up time, which is
 * the crystal start-up plus 37.74 us, with margin.
 */
inline void ptRfidHardReset(void) {
  pinMode(PT_RC522_RST, OUTPUT);
  digitalWrite(PT_RC522_RST, LOW);
  delay(10);
  digitalWrite(PT_RC522_RST, HIGH);
  delay(60);
}

/* Bring the bus up the way the official UNO Q SPI example does: SS is a plain
 * output held HIGH (deselected) before SPI.begin() claims the pins. The board's
 * devicetree sets st,gpio-control on SPI2, meaning chip select is a GPIO rather
 * than a peripheral function, so driving it manually is the intended usage. */
inline void ptSpiBusBegin(void) {
  pinMode(PT_RC522_SS, OUTPUT);
  digitalWrite(PT_RC522_SS, HIGH);
  SPI.begin();
}

/*
 * Transfer one raw byte at a chosen clock, with manual chip select.
 *
 * This is the primitive both the loopback test and the register reads are built
 * from. Keeping chip select explicit rather than letting the peripheral manage it
 * means the loopback test works with no slave present at all.
 */
inline uint8_t ptSpiXfer(uint8_t out, uint32_t clockHz) {
  uint8_t in;
  SPI.beginTransaction(SPISettings(clockHz, MSBFIRST, SPI_MODE0));
  digitalWrite(PT_RC522_SS, LOW);
  in = SPI.transfer(out);
  digitalWrite(PT_RC522_SS, HIGH);
  SPI.endTransaction();
  return in;
}

/*
 * Read one MFRC522 register over raw SPI.
 *
 * `rawAddr` is the unshifted datasheet address, for example 0x37 for VersionReg.
 * Per datasheet 8.1.2.3 the address byte is (addr << 1) & 0x7E with the MSB set
 * for a read. For 0x37 that yields 0xEE, byte for byte what the library sends, so
 * results from this path and from PCD_ReadRegister are directly comparable.
 *
 * A register read is two bytes on the wire: the address, then one dummy byte
 * during which the chip shifts out the value.
 */
inline uint8_t ptSpiReadReg(uint8_t rawAddr, uint32_t clockHz) {
  uint8_t addrByte = (uint8_t)(0x80 | ((rawAddr << 1) & 0x7E));
  SPI.beginTransaction(SPISettings(clockHz, MSBFIRST, SPI_MODE0));
  digitalWrite(PT_RC522_SS, LOW);
  SPI.transfer(addrByte);
  uint8_t value = SPI.transfer(0x00);
  digitalWrite(PT_RC522_SS, HIGH);
  SPI.endTransaction();
  return value;
}

/*
 * Classify what a set of reads tells us about the MISO line.
 *
 * WHY THIS EXISTS: "it returned 0x00" is not a diagnosis. The same byte can mean
 * a dead bus, an unpowered chip, a missing chip select, or a short to ground, and
 * each has a different fix. The distinguishing evidence is whether the returned
 * bytes VARY with what was sent. A line that is stuck returns the same value
 * regardless of stimulus; a line that is working echoes it.
 */
typedef enum {
  PT_MISO_OK = 0,      /* returns track what was sent            */
  PT_MISO_STUCK_LOW,   /* always 0x00 regardless of stimulus     */
  PT_MISO_STUCK_HIGH,  /* always 0xFF regardless of stimulus     */
  PT_MISO_NOISY        /* varies, but not in a way that matches  */
} PtMisoVerdict;

inline const char* ptMisoVerdictName(PtMisoVerdict v) {
  switch (v) {
    case PT_MISO_OK:         return "MISO tracks the stimulus";
    case PT_MISO_STUCK_LOW:  return "MISO stuck LOW";
    case PT_MISO_STUCK_HIGH: return "MISO stuck HIGH";
    case PT_MISO_NOISY:      return "MISO responds but does not match";
  }
  return "?";
}

/* Print a two-column sent/received table. R15: raw values always, never a bare
 * verdict, so the reader can form their own conclusion from the evidence. */
inline void ptPrintXferTable(const uint8_t* sent, const uint8_t* got,
                             uint8_t count) {
  logStep("      sent   received   match");
  for (uint8_t i = 0; i < count; ++i) {
    bool ok = (sent[i] == got[i]);
    ptLogf("      0x%02X   0x%02X       %s", sent[i], got[i], ok ? "yes" : "NO");
  }
}

#endif /* PT_SPI_PROBE_H */
