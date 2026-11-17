/*
 * PhishTap Lite — production firmware (Stage 1b)
 *
 * Single source of truth for every pin this project touches.
 *
 * WHY this file exists: the UNO Q pinout differs from the classic UNO in ways
 * that are easy to get wrong from memory (only six pins can do PWM, D10/D11 are
 * simultaneously SPI and PWM, and the onboard RGB LEDs are active-LOW while our
 * external LEDs are active-HIGH). Keeping the map in one place means a wiring
 * change is a one-line edit instead of a hunt through the sketch.
 *
 * Every entry below was checked against the official UNO Q pin tables in
 * docs.arduino.cc/tutorials/uno-q/user-manual (rev. 2026-09-25).
 */

#ifndef PT_PINS_H
#define PT_PINS_H

#include <Arduino.h>

/* ── MFRC522 / RC522 over SPI ───────────────────────────────────────────────
 * SS/MOSI/MISO/SCK are NOT free choices on this board: the UNO Q routes SPI1 to
 * PB9/PB15/PB14/PB13, which are exposed as D10/D11/D12/D13. Only RST is ours to
 * pick; D5 (PA11) is a plain GPIO here and its FDCAN1_RX alternate function is
 * unused by this project.
 *
 * The RC522 is a 3.3 V part. Its VCC must go to the 3V3 header pin. Feeding it
 * 5 V destroys the chip, so the wiring guide repeats that warning.
 *
 * IRQ is deliberately left unconnected: the production design polls the reader
 * from a non-blocking state machine, so an interrupt line would add a pin and a
 * race without removing any work.
 */
#define PT_RC522_SS    D10
#define PT_RC522_MOSI  D11
#define PT_RC522_MISO  D12
#define PT_RC522_SCK   D13
#define PT_RC522_RST   D5

/* ── Lives LEDs (external, on D7/D8/D9) ─────────────────────────────────────
 * Wired pin -> 150 R -> LED anode -> LED cathode -> GND, i.e. the pin SOURCES
 * current, so these are ACTIVE-HIGH. That is the opposite polarity to the
 * onboard RGB LEDs below; mixing the two up is the single most likely wiring
 * bug in this project, hence the explicit names.
 *
 * D7 (PB2) and D8 (PB4) are plain GPIO. D9 (PB8) also happens to be PWM-capable,
 * which is unused here but leaves the option of fading the last life later.
 */
#define PT_LIFE_LED_1  D7
#define PT_LIFE_LED_2  D8
#define PT_LIFE_LED_3  D9
#define PT_LIFE_ON     HIGH
#define PT_LIFE_OFF    LOW

/* ── Buzzer (TMB12A05, ACTIVE, via low-side NPN switch on D6) ───────────────
 * D6 (PB1) drives the transistor base through 1 kR; a 10 kR base-emitter
 * pulldown keeps the NPN off while the pin floats during reset, so the buzzer
 * cannot scream at power-on.
 *
 * The TMB12A05 has an internal oscillator, so it only needs ON/OFF. No PWM and
 * no tone(): the pitch is fixed by the part at roughly 2.3 kHz and cannot be
 * changed from software. All audio feedback is therefore rhythm-based.
 */
#define PT_BUZZER_PIN  D6
#define PT_BUZZER_ON   HIGH
#define PT_BUZZER_OFF  LOW

/* ── Reserved for the future physical PHISH/SAFE buttons (Stage 6) ──────────
 * Declared now so nothing else claims these pins later. INPUT_PULLUP with the
 * other side to GND means pressed == LOW and needs no external resistor.
 */
#define PT_BUTTON_A    D2
#define PT_BUTTON_B    D4
#define PT_BUTTON_PRESSED LOW

/* ── Onboard RGB LEDs (MCU-controlled, ACTIVE-LOW) ──────────────────────────
 * The core exposes these as LED3_R/G/B and LED4_R/G/B. They are confirmed
 * active-low: the official arduino/app-bricks-examples "unoq-pin-toggle" sketch
 * writes HIGH to all six segments in setup() specifically to turn them OFF, and
 * its Python side marks each one {"active_low": True}.
 *
 * The #ifdef fallbacks are deliberate. That same official example also refers to
 * the segments positionally as LED_BUILTIN, LED_BUILTIN+1 ... LED_BUILTIN+5, so
 * if a given core release does not export the named macros the offsets still
 * resolve to the same six pins and the selftest keeps compiling. PT_RGB_NAMED
 * records which path was taken so Phase A can report it.
 */
#if defined(LED3_R)
  #define PT_RGB3_R      LED3_R
  #define PT_RGB3_NAMED  1
#else
  #define PT_RGB3_R      LED_BUILTIN
  #define PT_RGB3_NAMED  0
#endif

#if defined(LED3_G)
  #define PT_RGB3_G      LED3_G
#else
  #define PT_RGB3_G      (LED_BUILTIN + 1)
#endif

#if defined(LED3_B)
  #define PT_RGB3_B      LED3_B
#else
  #define PT_RGB3_B      (LED_BUILTIN + 2)
#endif

#if defined(LED4_R)
  #define PT_RGB4_R      LED4_R
#else
  #define PT_RGB4_R      (LED_BUILTIN + 3)
#endif

#if defined(LED4_G)
  #define PT_RGB4_G      LED4_G
#else
  #define PT_RGB4_G      (LED_BUILTIN + 4)
#endif

#if defined(LED4_B)
  #define PT_RGB4_B      LED4_B
#else
  #define PT_RGB4_B      (LED_BUILTIN + 5)
#endif

/* Active-low: writing LOW lights the segment. */
#define PT_RGB_ON        LOW
#define PT_RGB_OFF       HIGH

/* ── Pins this project must never drive ─────────────────────────────────────
 * D0/D1 (PB7/PB6) are USART1. The Bridge itself does NOT use them — it rides an
 * internal UART that Linux exposes as /dev/ttyHS1 — but the project rule is to
 * leave them alone so no future change can collide with a serial peripheral.
 * A0-A5 are not 5 V tolerant when used as analog inputs.
 * JMEDIA / JMISC / JCTL are 1.8 V banks with a 2.1 V absolute maximum and must
 * never see a 3.3 V signal.
 */

#endif /* PT_PINS_H */
