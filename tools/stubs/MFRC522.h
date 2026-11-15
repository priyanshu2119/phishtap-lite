/*
 * Stub MFRC522.h for host-side syntax checking ONLY.
 *
 * Transcribed from miguelbalboa/rfid src/MFRC522.h (the library App Lab installs
 * as "MFRC522" by GithubCommunity). Every signature, enum value and member below
 * was read off the upstream header, not guessed. The API is identical in 1.4.11
 * and 1.4.12; the project pins **1.4.12** because 1.4.11's
 * MFRC522Extended.cpp does not compile on this core — it compares a `byte *`
 * with `> 0`, which is an error in C++. See tools/check_lib.sh.
 *
 * The card-MUTATING members are declared in full, on purpose. A syntax check
 * cannot enforce the project's read-only policy — referencing MIFARE_Write is
 * always legal C++ — so tools/no_write_check.sh does that job textually. Having
 * the symbols present here means the grep check is testing against a realistic
 * API rather than a fantasy one.
 */

#ifndef STUB_MFRC522_H
#define STUB_MFRC522_H

#include <Arduino.h>
#include <SPI.h>

/* Guarded exactly as upstream does, so a sketch can override it by defining the
 * macro BEFORE including this header. The selftest sets 1 MHz for jumper-wire
 * reliability; upstream defaults to 4 MHz. The chip accepts up to 10 MHz. */
#ifndef MFRC522_SPICLOCK
#define MFRC522_SPICLOCK (4000000u)
#endif

class MFRC522 {
public:
  static constexpr byte    FIFO_SIZE  = 64;
  static constexpr uint8_t UNUSED_PIN = UINT8_MAX;

  /* Register addresses are stored pre-shifted, because over SPI the address byte
   * is the register address shifted left by one (datasheet section 8.1.2.3). */
  enum PCD_Register : byte {
    CommandReg        = 0x01 << 1,
    ComIEnReg         = 0x02 << 1,
    DivIEnReg         = 0x03 << 1,
    ComIrqReg         = 0x04 << 1,
    DivIrqReg         = 0x05 << 1,
    ErrorReg          = 0x06 << 1,
    Status1Reg        = 0x07 << 1,
    Status2Reg        = 0x08 << 1,
    FIFODataReg       = 0x09 << 1,
    FIFOLevelReg      = 0x0A << 1,
    WaterLevelReg     = 0x0B << 1,
    ControlReg        = 0x0C << 1,
    BitFramingReg     = 0x0D << 1,
    CollReg           = 0x0E << 1,
    ModeReg           = 0x11 << 1,
    TxModeReg         = 0x12 << 1,
    RxModeReg         = 0x13 << 1,
    TxControlReg      = 0x14 << 1,
    TxASKReg          = 0x15 << 1,
    TxSelReg          = 0x16 << 1,
    RxSelReg          = 0x17 << 1,
    RxThresholdReg    = 0x18 << 1,
    DemodReg          = 0x19 << 1,
    MfTxReg           = 0x1C << 1,
    MfRxReg           = 0x1D << 1,
    SerialSpeedReg    = 0x1F << 1,
    CRCResultRegH     = 0x21 << 1,
    CRCResultRegL     = 0x22 << 1,
    ModWidthReg       = 0x24 << 1,
    RFCfgReg          = 0x26 << 1,
    GsNReg            = 0x27 << 1,
    CWGsPReg          = 0x28 << 1,
    ModGsPReg         = 0x29 << 1,
    TModeReg          = 0x2A << 1,
    TPrescalerReg     = 0x2B << 1,
    TReloadRegH       = 0x2C << 1,
    TReloadRegL       = 0x2D << 1,
    TCounterValueRegH = 0x2E << 1,
    TCounterValueRegL = 0x2F << 1,
    TestSel1Reg       = 0x31 << 1,
    TestSel2Reg       = 0x32 << 1,
    TestPinEnReg      = 0x33 << 1,
    TestPinValueReg   = 0x34 << 1,
    TestBusReg        = 0x35 << 1,
    AutoTestReg       = 0x36 << 1,
    VersionReg        = 0x37 << 1,
    AnalogTestReg     = 0x38 << 1,
    TestDAC1Reg       = 0x39 << 1,
    TestDAC2Reg       = 0x3A << 1,
    TestADCReg        = 0x3B << 1
  };

  enum PCD_Command : byte {
    PCD_Idle               = 0x00,
    PCD_Mem                = 0x01,
    PCD_GenerateRandomID   = 0x02,
    PCD_CalcCRC            = 0x03,
    PCD_Transmit           = 0x04,
    PCD_NoCmdChange        = 0x07,
    PCD_Receive            = 0x08,
    PCD_Transceive         = 0x0C,
    PCD_MFAuthent          = 0x0E,
    PCD_SoftReset          = 0x0F
  };

  enum PCD_RxGain : byte {
    RxGain_18dB   = 0x00 << 4,
    RxGain_23dB   = 0x01 << 4,
    RxGain_18dB_2 = 0x02 << 4,
    RxGain_23dB_2 = 0x03 << 4,
    RxGain_33dB   = 0x04 << 4,
    RxGain_38dB   = 0x05 << 4,
    RxGain_43dB   = 0x06 << 4,
    RxGain_48dB   = 0x07 << 4,
    RxGain_min    = 0x00 << 4,
    RxGain_avg    = 0x04 << 4,
    RxGain_max    = 0x07 << 4
  };

  enum PICC_Command : byte {
    PICC_CMD_REQA          = 0x26,
    PICC_CMD_WUPA          = 0x52,
    PICC_CMD_CT            = 0x88,
    PICC_CMD_SEL_CL1       = 0x93,
    PICC_CMD_SEL_CL2       = 0x95,
    PICC_CMD_SEL_CL3       = 0x97,
    PICC_CMD_HLTA          = 0x50,
    PICC_CMD_RATS          = 0xE0,
    PICC_CMD_MF_AUTH_KEY_A = 0x60,
    PICC_CMD_MF_AUTH_KEY_B = 0x61,
    PICC_CMD_MF_READ       = 0x30,
    /* The five below mutate card contents and must never be sent by this
     * project. Declared so the API is faithful; policed by no_write_check.sh. */
    PICC_CMD_MF_WRITE      = 0xA0,
    PICC_CMD_MF_DECREMENT  = 0xC0,
    PICC_CMD_MF_INCREMENT  = 0xC1,
    PICC_CMD_MF_RESTORE    = 0xC2,
    PICC_CMD_MF_TRANSFER   = 0xB0,
    PICC_CMD_UL_WRITE      = 0xA2
  };

  enum MIFARE_Misc {
    MF_ACK       = 0xA,
    MF_KEY_SIZE  = 6
  };

  enum PICC_Type : byte {
    PICC_TYPE_UNKNOWN = 0,
    PICC_TYPE_ISO_14443_4,
    PICC_TYPE_ISO_18092,
    PICC_TYPE_MIFARE_MINI,
    PICC_TYPE_MIFARE_1K,
    PICC_TYPE_MIFARE_4K,
    PICC_TYPE_MIFARE_UL,
    PICC_TYPE_MIFARE_PLUS,
    PICC_TYPE_MIFARE_DESFIRE,
    PICC_TYPE_TNP3XXX,
    PICC_TYPE_NOT_COMPLETE = 0xff
  };

  /* STATUS_OK is the FIRST enumerator, so it is 0. That is the detail the
   * previous build got wrong: `if (!PICC_Select())` reads as "if not OK", which
   * inverts the test. Always compare against STATUS_OK explicitly. */
  enum StatusCode : byte {
    STATUS_OK = 0,
    STATUS_ERROR,
    STATUS_COLLISION,
    STATUS_TIMEOUT,
    STATUS_NO_ROOM,
    STATUS_INTERNAL_ERROR,
    STATUS_INVALID,
    STATUS_CRC_WRONG,
    STATUS_MIFARE_NACK = 0xff
  };

  typedef struct {
    byte size;            /* 4, 7 or 10 */
    byte uidByte[10];
    byte sak;
  } Uid;

  typedef struct {
    byte keyByte[MF_KEY_SIZE];
  } MIFARE_Key;

  Uid uid;

  MFRC522() : _chipSelectPin(UNUSED_PIN), _resetPowerDownPin(UNUSED_PIN) {}
  MFRC522(byte resetPowerDownPin)
      : _chipSelectPin(UNUSED_PIN), _resetPowerDownPin(resetPowerDownPin) {}
  MFRC522(byte chipSelectPin, byte resetPowerDownPin)
      : _chipSelectPin(chipSelectPin), _resetPowerDownPin(resetPowerDownPin) {}

  /* ── Basic register interface ─────────────────────────────────────────── */
  void PCD_WriteRegister(PCD_Register reg, byte value) { (void)reg; (void)value; }
  void PCD_WriteRegister(PCD_Register reg, byte count, byte *values) {
    (void)reg; (void)count; (void)values;
  }
  byte PCD_ReadRegister(PCD_Register reg) { (void)reg; return 0; }
  void PCD_ReadRegister(PCD_Register reg, byte count, byte *values, byte rxAlign = 0) {
    (void)reg; (void)count; (void)values; (void)rxAlign;
  }
  void PCD_SetRegisterBitMask(PCD_Register reg, byte mask) { (void)reg; (void)mask; }
  void PCD_ClearRegisterBitMask(PCD_Register reg, byte mask) { (void)reg; (void)mask; }
  StatusCode PCD_CalculateCRC(byte *data, byte length, byte *result) {
    (void)data; (void)length; (void)result; return STATUS_OK;
  }

  /* ── Chip control ───────────────────────────────────────────────────────
   * PCD_Init does NOT call SPI.begin(); the sketch must do that first.
   * It does perform a reset, set the antenna gain to RxGain_max and turn the
   * antenna on. */
  void PCD_Init() {}
  void PCD_Init(byte resetPowerDownPin) { (void)resetPowerDownPin; }
  void PCD_Init(byte chipSelectPin, byte resetPowerDownPin) {
    (void)chipSelectPin; (void)resetPowerDownPin;
  }
  void PCD_Reset() {}
  void PCD_AntennaOn() {}
  void PCD_AntennaOff() {}
  byte PCD_GetAntennaGain() { return RxGain_max; }
  void PCD_SetAntennaGain(byte mask) { (void)mask; }

  /* Drives the chip's internal test bus and compares against reference firmware
   * images. Leaves the test registers modified, so PCD_Init() must be called
   * again afterwards. Returns false on FM17522 clones (version 0x88) because
   * their values do not match the NXP tables, even though they read cards fine. */
  bool PCD_PerformSelfTest() { return true; }

  void PCD_SoftPowerDown() {}
  void PCD_SoftPowerUp() {}

  /* ── PICC (card) communication ────────────────────────────────────────── */
  StatusCode PCD_TransceiveData(byte *sendData, byte sendLen, byte *backData,
                                byte *backLen, byte *validBits = nullptr,
                                byte rxAlign = 0, bool checkCRC = false) {
    (void)sendData; (void)sendLen; (void)backData; (void)backLen;
    (void)validBits; (void)rxAlign; (void)checkCRC; return STATUS_OK;
  }
  StatusCode PCD_CommunicateWithPICC(byte command, byte waitIRq, byte *sendData,
                                     byte sendLen, byte *backData = nullptr,
                                     byte *backLen = nullptr,
                                     byte *validBits = nullptr, byte rxAlign = 0,
                                     bool checkCRC = false) {
    (void)command; (void)waitIRq; (void)sendData; (void)sendLen; (void)backData;
    (void)backLen; (void)validBits; (void)rxAlign; (void)checkCRC;
    return STATUS_OK;
  }

  /* REQA. bufferSize must be 2 on entry. Returns STATUS_COLLISION when more
   * than one card answers, which still means "a card is present". */
  StatusCode PICC_RequestA(byte *bufferATQA, byte *bufferSize) {
    (void)bufferATQA; (void)bufferSize; return STATUS_OK;
  }
  /* WUPA. The ONLY way to re-detect a card that is already in HALT state: a
   * halted card does not answer REQA, so PICC_IsNewCardPresent() reports
   * "absent" while the card is still sitting on the reader. */
  StatusCode PICC_WakeupA(byte *bufferATQA, byte *bufferSize) {
    (void)bufferATQA; (void)bufferSize; return STATUS_OK;
  }
  StatusCode PICC_REQA_or_WUPA(byte command, byte *bufferATQA, byte *bufferSize) {
    (void)command; (void)bufferATQA; (void)bufferSize; return STATUS_OK;
  }
  /* Performs the anticollision SELECT. Call exactly once per detection; sending
   * a second REQA before SELECT is what broke the previous build. */
  virtual StatusCode PICC_Select(Uid *uid, byte validBits = 0) {
    (void)uid; (void)validBits; return STATUS_OK;
  }
  StatusCode PICC_HaltA() { return STATUS_OK; }

  /* Convenience wrappers, declared virtual upstream. PICC_IsNewCardPresent()
   * sends REQA and already treats STATUS_COLLISION as "a card answered", so a
   * collision is a hit rather than an error. PICC_ReadCardSerial() performs the
   * SELECT and populates the public `uid` member. */
  virtual bool PICC_IsNewCardPresent() { return false; }
  virtual bool PICC_ReadCardSerial() { return false; }

  /* ── MIFARE ───────────────────────────────────────────────────────────── */
  StatusCode PCD_Authenticate(byte command, byte blockAddr, MIFARE_Key *key, Uid *uid) {
    (void)command; (void)blockAddr; (void)key; (void)uid; return STATUS_OK;
  }
  void PCD_StopCrypto1() {}

  /* Read-only. Allowed by the project policy. */
  StatusCode MIFARE_Read(byte blockAddr, byte *buffer, byte *bufferSize) {
    (void)blockAddr; (void)buffer; (void)bufferSize; return STATUS_OK;
  }
  StatusCode MIFARE_GetValue(byte blockAddr, int32_t *value) {
    (void)blockAddr; (void)value; return STATUS_OK;
  }

  /* ── FORBIDDEN BY PROJECT POLICY ────────────────────────────────────────
   * Everything below this line can modify card contents. Declared for API
   * fidelity only. tools/no_write_check.sh fails the build if any first-party
   * source references one of these names outside a comment. */
  StatusCode MIFARE_Write(byte blockAddr, byte *buffer, byte bufferSize) {
    (void)blockAddr; (void)buffer; (void)bufferSize; return STATUS_OK;
  }
  StatusCode MIFARE_Ultralight_Write(byte page, byte *buffer, byte bufferSize) {
    (void)page; (void)buffer; (void)bufferSize; return STATUS_OK;
  }
  StatusCode MIFARE_Decrement(byte blockAddr, int32_t delta) {
    (void)blockAddr; (void)delta; return STATUS_OK;
  }
  StatusCode MIFARE_Increment(byte blockAddr, int32_t delta) {
    (void)blockAddr; (void)delta; return STATUS_OK;
  }
  StatusCode MIFARE_Restore(byte blockAddr) { (void)blockAddr; return STATUS_OK; }
  StatusCode MIFARE_Transfer(byte blockAddr) { (void)blockAddr; return STATUS_OK; }
  StatusCode MIFARE_SetValue(byte blockAddr, int32_t value) {
    (void)blockAddr; (void)value; return STATUS_OK;
  }
  void MIFARE_SetAccessBits(byte *accessBitBuffer, byte g0, byte g1, byte g2, byte g3) {
    (void)accessBitBuffer; (void)g0; (void)g1; (void)g2; (void)g3;
  }
  bool MIFARE_OpenUidBackdoor(bool logErrors) { (void)logErrors; return false; }
  bool MIFARE_SetUid(byte *newUid, byte uidSize, bool logErrors) {
    (void)newUid; (void)uidSize; (void)logErrors; return false;
  }
  bool MIFARE_UnbrickUidSector(bool logErrors) { (void)logErrors; return false; }
  StatusCode PCD_NTAG216_AUTH(byte *passWord, byte pACK[]) {
    (void)passWord; (void)pACK; return STATUS_OK;
  }

  /* ── Support ────────────────────────────────────────────────────────────
   * Both name helpers return __FlashStringHelper*, an AVR PROGMEM abstraction
   * with no meaning on ARM/Zephyr. The selftest deliberately does not call them
   * and maps the enums to plain const char* itself. */
  static const __FlashStringHelper *GetStatusCodeName(StatusCode code) {
    (void)code; return nullptr;
  }
  static PICC_Type PICC_GetType(byte sak) { (void)sak; return PICC_TYPE_UNKNOWN; }
  static const __FlashStringHelper *PICC_GetTypeName(PICC_Type type) {
    (void)type; return nullptr;
  }

  /* Debug helpers that write to Serial. Avoided for the same reason. */
  void PCD_DumpVersionToSerial() {}
  void PICC_DumpToSerial(Uid *uid) { (void)uid; }
  void PICC_DumpDetailsToSerial(Uid *uid) { (void)uid; }
  void PICC_DumpMifareClassicToSerial(Uid *uid, PICC_Type piccType, MIFARE_Key *key) {
    (void)uid; (void)piccType; (void)key;
  }
  void PICC_DumpMifareClassicSectorToSerial(Uid *uid, MIFARE_Key *key, byte sector) {
    (void)uid; (void)key; (void)sector;
  }
  void PICC_DumpMifareUltralightToSerial() {}

  StatusCode PCD_MIFARE_Transceive(byte *sendData, byte sendLen, bool acceptTimeout = false) {
    (void)sendData; (void)sendLen; (void)acceptTimeout; return STATUS_OK;
  }

protected:
  byte _chipSelectPin;
  byte _resetPowerDownPin;
  StatusCode MIFARE_TwoStepHelper(byte command, byte blockAddr, int32_t data) {
    (void)command; (void)blockAddr; (void)data; return STATUS_OK;
  }
};

#endif /* STUB_MFRC522_H */
