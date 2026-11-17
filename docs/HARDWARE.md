# PhishTap Lite — Hardware Notes

Everything here was checked against a primary source: the official UNO Q
documentation, the actual headers on the board, or a component datasheet. Where
something could not be verified it says so.

---

## Bill of materials

| Item | Spec | Status |
|---|---|---|
| Arduino UNO Q | 4 GB RAM, 32 GB eMMC | in hand |
| MFRC522 / RC522 | 13.56 MHz ISO14443A, SPI, 3.3 V | in hand |
| MIFARE Classic 1K white card | 4-byte UID, 13.56 MHz | in hand, confirmed working |
| Blue keyfob | frequency unknown | unverified, see below |
| TMB12A05 buzzer | active, 5 V, electromagnetic | in hand |
| 3x LED | red assumed, Vf ~2.0 V | in hand |
| 3x 150 R resistor | for the LEDs | in hand |
| 2N2222A or BC547 | NPN, TO-92 | in hand |
| 1 kR, 10 kR | base and pulldown | in hand |
| 1N4007 | flyback diode | in hand |
| 5 V / 3 A USB-C charger | | in hand |
| Laptop | EndeavourOS, USB-C, Wi-Fi | in hand |

No monitor or TV is available at the venue, so the kiosk is the laptop running a
fullscreen browser tab.

---

## TMB12A05 buzzer

From the datasheet:

| Spec | Value |
|---|---|
| Type | active electromagnetic, internal drive circuit |
| Rated voltage | 5 V DC |
| Operating range | 4 to 7 V DC |
| Rated current | <= 30 mA, measured 20.3 mA at 5 V |
| Resonant frequency | 2300 Hz +/- 500, fixed |
| Sound output | >= 85 dB at 10 cm, measured 88.9 dB at 5 V |
| Package | 12 mm dia x 9.5 mm, 7.6 mm pin pitch |

The measured curve from the datasheet:

| Voltage | Sound | Current |
|---|---|---|
| 4.0 V | 86.3 dB | 16.3 mA |
| 4.5 V | 87.9 dB | 18.3 mA |
| 5.0 V | 88.9 dB | 20.3 mA |
| 5.5 V | 89.7 dB | 22.3 mA |
| 6.0 V | 90.4 dB | 25.3 mA |

Three consequences that shaped the design:

**It is driven with `digitalWrite` only.** There is an oscillator inside, so there
is no frequency to generate. No PWM and no `tone()`. This turned out to be lucky
rather than merely convenient, because this board's PWM is not a usable audio
clock.

**The pitch cannot be changed.** Every cue has to be distinguished by rhythm alone.
The pattern table lives in `pt_buzzer.h`.

**It needs the 5 V rail.** 3.3 V is below the 4.0 V minimum and it will not
oscillate. The transistor is still required despite the low current, because
20.3 mA is at the pin budget and the part is a 5 V device on 3.3 V logic.

The part is polarised. The case is marked `+` and that leg goes to the 5 V rail.
Reversed, it stays silent.

### Sanity check

Touch the buzzer directly across 5 V and GND with the correct polarity. A steady
continuous tone means it is active and working. A single click means it is passive
or dead.

### Volume

Do not add a series resistor. At ~20 mA a 47 R resistor drops ~0.95 V, which with
the transistor's saturation drop can put the buzzer below its 4.0 V minimum and it
stops oscillating entirely. If it is too loud, shorten the beeps or damp the sound
hole. If a resistor is tried at all, use 10 to 22 R and only after measuring the
voltage across the buzzer with a multimeter to confirm it stays at or above 4.0 V.

Never reduce volume with PWM. A 500 Hz square wave amplitude-modulating a 2300 Hz
tone sounds terrible.

---

## The blue keyfob is unverified

The RC522 reads only ISO14443A at 13.56 MHz. It can never read a 125 kHz LF tag,
and a 125 kHz reader can never read a 13.56 MHz tag. The two bands are mutually
exclusive.

The white MIFARE Classic 1K card is confirmed 13.56 MHz and works. The blue keyfob
is unknown, and cheap blue fobs are very often 125 kHz EM4100, which the RC522
cannot see at all.

Ways to identify it:

- **The marking.** `IC`, MIFARE, S50, 1K, NTAG or NFC means 13.56 MHz. `ID`, EM,
  EM4100, TK4100 or T5577 means 125 kHz.
- **An Android phone's NFC reader.** It detects 13.56 MHz only. A notification or
  vibration means the fob will work. Never use an iPhone for this test, iOS NFC
  reading is too restricted and gives false negatives.
- **Shining a light through a card.** Many thin turns with no visible gaps means
  125 kHz. Fewer thicker turns with clear gaps means 13.56 MHz. This does not work
  on an opaque fob.
- **Definitively, by tapping it during Phase B.**

If the fob turns out to be 125 kHz, drop it and use only the white card. The demo
uses a single card anyway. Do not add an RDM6300, one reader type is enough.

The UNO Q has no onboard RFID or NFC reader. Its only radio is the WCBN3536A,
which is Wi-Fi 5 and Bluetooth 5.1 at 2.4 GHz. Bluetooth is not RFID and cannot
substitute for the reader. The external RC522 is mandatory.

---

## Power

### JANALOG header order

The physical order matters because the 3V3 pin is crowded by control and power
pins. From the UNO Q datasheet:

| Pin | Name | Warning |
|---|---|---|
| 1 | **BOOT** | MCU_BOOT0; a misplaced jumper can enter bootloader mode |
| 2 | **IOREF** | 3.3 V output/reference, not the RC522 supply pin |
| 3 | **RESET** | MCU_NRST; a misplaced jumper can reboot the board |
| 4 | **+3V3** | The RC522 supply pin |
| 5 | **+5V** | 5V_SYS; never connect this to RC522 VCC |
| 6 | **GND** | |
| 7 | **GND** | |
| 8 | **VIN** | 7-24 V input, not an output |
| 9+ | A0-A5 | Analog inputs, not 5 V tolerant |

The +3V3 pin sits between RESET and +5V. Do not count positions from memory:
read the silkscreen and verify the rail with the LED test in the selftest README.
The same 3V3 rail is available on JMISC and JMEDIA, but those headers also carry
1.8 V processor I/O and are not suitable for this wiring.

From the official power specification:

| Input | Voltage | Current |
|---|---|---|
| USB-C VBUS | 5 V | >= 3 A |
| VIN (DC IN) | 7 to 24 V | sized to the 5 V budget |
| 5V pin | 5 V | >= 3 A |

The board negotiates a 5 V / 3 A PD contract only and never asks for a
higher-voltage profile. USB-C VBUS and the VIN buck output are diode-OR'd into
`5V_SYS`, which feeds a 3.8 V pre-regulator stage and then the 3.3 V rail. The
PMIC derives 1.8 V from `5V_SYS`.

Recommended operating conditions: USB-C input 4.5 to 5.5 V, 3.3 V rail 3.1 to
3.5 V, ambient -10 to 60 C.

The documentation is explicit that an unstable supply causes resets:

> Use a stable 5 V / 3 A source. If the supply current-limits during peaks,
> voltage hang may cause resets.

A laptop USB port is marginal. USB-A 2.0 gives 500 mA, USB-A 3.0 gives 900 mA, and
USB-C without PD gives around 1.5 A. Bring-up can run from the laptop, but the demo
runs from the wall charger. If random resets or Wi-Fi drops appear on laptop power,
switch to the charger.

Header current is shared with the onboard loads and no fixed figure is published,
so external peripherals have to be budgeted against whatever the board is drawing
at the time. Our worst case is roughly 100 mA of RF transmit burst plus 26 mA of
LEDs plus 20 mA of buzzer, which is comfortable on a 3 A supply and marginal on a
laptop port.

### Voltage domains

| Header | Domain | Absolute max |
|---|---|---|
| JDIGITAL, JANALOG, QWIIC, JSPI | 3.3 V maker I/O | 3.6 V |
| JMEDIA, JMISC, JCTL | 1.8 V processor I/O | 2.1 V |

Never drive the 1.8 V banks with a 3.3 V signal. A0 and A1 are ADC inputs only and
must not source or sink DC current. They are not 5 V tolerant.

The STM32U585's SPI pins are 5 V tolerant as inputs but drive 3.3 V as outputs,
which is why the RC522 connects directly with no level shifting.

---

## Network

### Option A, laptop hotspot (default)

The EndeavourOS laptop becomes the access point. On Arch, NetworkManager
connection sharing needs `dnsmasq` installed, and the interface is named something
like `wlp2s0`, not `wlan0`.

```bash
sudo pacman -S dnsmasq
nmcli device status                    # find the Wi-Fi interface
iw list | grep -A8 "Supported interface modes"   # confirm AP support
sudo nmcli device wifi hotspot ifname wlp2s0 con-name PhishTap \
     ssid PhishTap password phishtap123
```

NetworkManager's shared mode uses dnsmasq with client isolation off by default, so
the phone can reach the board on port 7000. The laptop becomes the gateway at
10.42.0.1 and can itself reach the board for the kiosk browser. There is no
captive portal. The laptop loses internet, which does not matter because the app is
fully offline. Do all downloads, cores and libraries before switching.

### Option B, college or hostel Wi-Fi

The development network is "MUIT BOYS HOSTEL", multiple routers on one SSID. Four
things can break it:

1. **Client or AP isolation**, so the phone cannot reach the board on port 7000.
   This kills the demo outright and is common on shared networks.
2. **A captive portal.** The board has no browser, so it cannot authenticate.
3. **Per-router NAT.** If the routers are not properly bridged, the board and the
   phone can land on different subnets with no route between them.
4. **mDNS blocked across subnets**, so App Lab's Network Mode discovery fails.

WPA2-Enterprise is supported by the board if the network uses it:

```bash
nmcli con add type wifi connection.id eduroam wifi.ssid eduroam \
  wifi.mode infrastructure wifi-sec.key-mgmt wpa-eap 802-1x.eap peap \
  802-1x.phase2-auth mschapv2 802-1x.identity <user>
```

### Mandatory pre-flight test

Do this the day before, not on the day.

```bash
# on the board
sudo nmcli d wifi connect "MUIT BOYS HOSTEL" password "<password>"
hostname -I

# from the laptop
curl http://<board-ip>:7000/api/health

# from the PHONE, on the same Wi-Fi, in a browser
http://<board-ip>:7000/api/health
```

If the phone times out while the laptop works, that is client isolation. Fall back
to option A.

### Option C, the board as its own access point

Unverified. Treat as unsupported and do not design around it.

### ADB

ADB over USB-C works with no network at all and is the preferred debug path:

```bash
adb devices
adb shell
```

`android-tools` provides it on Arch. The udev rules must be installed first or the
device will not be accessible.

---

## Verified platform facts

Read off the board, not from documentation.

| Item | Value |
|---|---|
| Zephyr core | 1.1.0 |
| Python | 3.13.18 |
| Arduino_LED_Matrix | 0.1.3, bundled in the core |
| SPI | 1.0.0, bundled in the core |
| Arduino_RouterBridge | 0.4.4, downloaded to `.arduino15/internal/` |
| Arduino_RPClite | 0.3.1 |
| MsgPack | 0.4.2 |
| DebugLog | 0.8.4 |
| ArxTypeTraits | 0.3.2 |
| ArxContainer | 0.7.0 |
| MFRC522 | 1.4.12, see below |
| Flash used by the selftest | 111348 bytes, 14% of 786432 |
| RAM used by the selftest | 45480 bytes, 17% of 262144 |
| ArduinoGraphics | **not bundled** |

Bundled core libraries: `Arduino_LED_Matrix`, `CAN`, `ea_malloc`, `RTC`,
`SocketWrapper`, `SPI`, `stubs`, `Wire`. Note that `Arduino_RouterBridge` is not
among them; it is fetched on demand.

Because `ArduinoGraphics` is absent, `MATRIX_WITH_ARDUINOGRAPHICS` is never
defined, the matrix class does not inherit from it, and `beginText`, `endText`,
`renderBitmap` and `set(x, y, ...)` are all unavailable. The hand-rolled 5x7 font
in `pt_font.h` is required, not redundant.

### LED matrix

The class is `Arduino_LED_Matrix`, with underscores. The header also ends with
`typedef Arduino_LED_Matrix ArduinoLEDMatrix;` so both spellings compile.

```cpp
void draw(const uint8_t *buf);        // 104 bytes, indexed [row * 13 + col]
void setGrayscaleBits(uint8_t max);   // 1, 3 or 8
int  begin(void);
```

The panel is 8 rows by 13 columns of blue LEDs, driven by the STM32 and not
reachable from the Linux side. The IRQ routine supports 3 bits of depth natively;
asking for 8 lets the library map a 0 to 255 range down.

The header also declares an `extern "C"` global API:

```cpp
extern "C" {
void matrixBegin(void);
void matrixEnd(void);
void matrixPlay(const uint8_t *buf, uint32_t len);
void matrixSetGrayscaleBits(uint8_t _max);
void matrixGrayscaleWrite(const uint8_t *buf);
void matrixWrite(const uint32_t *buf);
};
```

Defining a function with any of those names in a sketch produces
`conflicting declaration ... with 'C' linkage` and kills the build. Every matrix
helper in this project is therefore prefixed `ptMatrix`. The global `reverse()` is
also taken.

### SPI2 pin verification

The installed UNO Q core's `variant.h` defines the Arduino SPI aliases directly:

```cpp
#define MOSI D11 /* PB15 */
#define MISO D12 /* PB14 */
#define SCK  D13 /* PB13 */
#define SS   D10 /* PB9 */
```

The board overlay configures `spi2` with deferred initialization and GPIO chip
select control. This is why the project calls `SPI.begin()` and drives D10 as a
plain GPIO around each raw transfer. Phase L proves these mappings on the actual
board instead of relying on the header alone.

`ZephyrSPI::begin()` was read directly from the installed core
(`libraries/SPI/SPI.cpp`). It is not a one-time initializer; it unconditionally
re-applies the pinctrl every call:

```cpp
void arduino::ZephyrSPI::begin() {
    /* Re-apply DEFAULT pinctrl state so shared pins
     * are remuxed back to SPI after other peripherals have used them. */
    (void)zephyr::arduino::init_dev_apply_pinctrl(spi_dev);
}
```

This matters for Phase W, which reconfigures D12 as a plain GPIO input to probe
the MISO line state. Calling `SPI.begin()` afterwards remuxes D11/D12/D13 back to
SPI, so a later Phase B works without a reboot. There is no already-initialized
guard, so re-calling it is always safe. Phase L passing on real hardware is the
empirical confirmation that this remux works.

The installed core also defines `INPUT_PULLDOWN = 0x3`, and its
`wiring_digital.cpp` maps that mode to Zephyr's `GPIO_PULL_DOWN`. Phase W uses it
only after this check was made; it does not assume the constant exists on every
future core.

### RGB LEDs

From the datasheet: LED3 is PH10/PH11/PH12 for R/G/B, LED4 is PH13/PH14/PH15.
Confirmed active-low, meaning writing LOW lights the segment. The official
`unoq-pin-toggle` example writes HIGH to all six in `setup()` specifically to turn
them off, and its Python side marks each one `active_low: True`.

`LED_BUILTIN` equals `LED3_R`, and the six segments are contiguous from there.
`pt_pins.h` uses the named macros when they exist and falls back to the offsets,
and Phase A reports which path was taken.

LED1 and LED2 belong to Linux and are reached through `/sys/class/leds` or the
`Leds` helper in `arduino.app_utils`, not from a sketch.

### Serial and Monitor are the same object

`variants/arduino_uno_q_stm32u585xx/variant.h` defines
`ARDUINO_ROUTERBRIDGE_PROVIDES_SERIAL`, and `Arduino_RouterBridge`'s
`singletons.cpp` then declares:

```cpp
BridgeMonitor<> Monitor(Bridge);
extern BridgeMonitor<> Serial [[gnu::alias("Monitor")]];
```

So writing to either reaches App Lab's Serial monitor tab. `BridgeMonitor::begin()`
also explicitly discards both of its arguments:

```cpp
bool begin(unsigned long _legacy_baud=0, uint16_t _legacy_config=0) {
    (void)_legacy_baud;
    (void)_legacy_config;
```

The channel is virtual and carried over the router, so the fixed 9600 baud shown in
the UI is cosmetic and cannot be changed.

`begin()` self-heals: if the bridge is not started it calls `bridge->begin()`
itself, then queries `mon/connected`.

---

## MFRC522 library

### Pin 1.4.12, never 1.4.11

1.4.11 does not compile on this core. `src/MFRC522Extended.cpp` lines 824 and 847
contain:

```cpp
if (backData && (backLen > 0)) {     // backLen is a byte *
```

An ordered comparison between a pointer and an integer is an error in C++, not a
warning:

```
error: ordered comparison of pointer with integer zero
       ('byte*' {aka 'unsigned char*'} and 'int')
```

1.4.12 fixes it by dereferencing, `(*backLen > 0)`.

The project only uses the base `MFRC522` class and never touches
`MFRC522Extended`, but arduino-cli compiles every `.cpp` in a library's `src/`, so
the broken file is built anyway and takes the whole sketch down with it.

`tools/check_lib.sh` downloads the exact zip App Lab installs and syntax-checks it,
which is how this was confirmed without burning flash cycles.

The architecture warning is expected and harmless. 1.4.12's `library.properties`
lists `avr, megaavr, STM32F1, teensy, esp8266, esp32, samd, atmelsam, rp2040` and
still omits `zephyr`, so App Lab prints:

```
WARNING: library MFRC522 claims to run on avr, ... and may be incompatible with
your current board which runs on zephyr architecture(s).
```

The library is plain C++ over SPI and builds fine regardless.

### The SPI clock cannot be lowered from a sketch

`MFRC522_SPICLOCK` is guarded with `#ifndef` in the header, so it looks
overridable. It is not:

```
MFRC522.h:21     #ifndef MFRC522_SPICLOCK
MFRC522.h:22     #define MFRC522_SPICLOCK (4000000u)
MFRC522.cpp:49,65,82,106    SPI.beginTransaction(SPISettings(MFRC522_SPICLOCK, ...))
```

Every transaction lives in `MFRC522.cpp`, which is its own translation unit and
includes the header without the sketch's define. The library always runs at 4 MHz.
To test a slower clock you have to drive SPI yourself, which is what the selftest's
B0 raw probe does.

### Register reset values used as a fingerprint

From the NXP MFRC522 product data sheet, **Rev. 3.9, 27 April 2016**. The datasheet
documents each register as `Table N. <Reg> register (address XXh); reset value:
YYh`, so these are transcribed rather than remembered.

| Register | Address | Reset value | Notes |
|---|---|---|---|
| CommandReg | 0x01 | **0x20** | bit 5 RcvOff = 1, bit 4 PowerDown = 0 |
| ComIEnReg | 0x02 | 0x80 | |
| ErrorReg | 0x06 | 0x00 | |
| Status1Reg | 0x07 | 0x21 | low bits reflect CRC and timer state and can move |
| Status2Reg | 0x08 | 0x00 | |
| FIFOLevelReg | 0x0A | 0x00 | |
| WaterLevelReg | 0x0B | 0x08 | |
| ControlReg | 0x0C | 0x10 | |
| BitFramingReg | 0x0D | 0x00 | |
| ModeReg | 0x11 | **0x3F** | PCD_Init overwrites this to 0x3D |
| TxModeReg | 0x12 | 0x00 | |
| RxModeReg | 0x13 | 0x00 | |
| TxControlReg | 0x14 | 0x80 | |
| TxASKReg | 0x15 | 0x00 | PCD_Init overwrites this to 0x40 |
| TxSelReg | 0x16 | 0x10 | |
| RxSelReg | 0x17 | 0x84 | |
| RxThresholdReg | 0x18 | 0x84 | |
| DemodReg | 0x19 | **0x4D** | a distinctive bit pattern, good for diagnostics |
| SerialSpeedReg | 0x1F | 0xEB | |
| RFCfgReg | 0x26 | **0x48** | RxGain = 33 dB. PCD_Init overwrites to 0x70 |
| GsNReg | 0x27 | 0x88 | |
| CWGsPReg | 0x28 | 0x20 | |
| ModGsPReg | 0x29 | 0x20 | |
| TModeReg | 0x2A | 0x00 | PCD_Init overwrites to 0x80 |
| TPrescalerReg | 0x2B | 0x00 | PCD_Init overwrites to 0xA9 |
| TestSel1Reg | 0x31 | 0x00 | |
| TestSel2Reg | 0x32 | 0x00 | |
| TestPinEnReg | 0x33 | 0x80 | |
| TestPinValueReg | 0x34 | 0x00 | |
| AutoTestReg | 0x36 | 0x40 | |
| VersionReg | 0x37 | no reset value listed | section 9.3.4.8: v1.0 = **0x91**, v2.0 = **0x92**. FM17522 clones read **0x88** |
| AnalogTestReg | 0x38 | 0x00 | |

`DivIrqReg`, `CollReg`, `TReloadRegH`, `TReloadRegL` and `TestBusReg` have no
`reset value` entry in the datasheet tables, so they are not used as fingerprint
registers.

**Why this matters for diagnostics.** Ten registers with ten different documented
defaults form a fingerprint. A broken bus returning all 0x00 is unambiguous, and
getting all ten correct by chance on a noisy line is not realistic. A single
VersionReg read cannot make that distinction, because one register can read 0x00
for reasons unrelated to the chip being absent.

The fingerprint must be read **before** `PCD_Init()`, because that function
overwrites ModeReg, TxASKReg, TModeReg, TPrescalerReg, TReloadRegH/L and RFCfgReg.
Reading the same registers *after* `PCD_Init()` is also useful, but for the
opposite reason: it proves that writes reach the chip and not only reads.

### Counterfeit modules are normal, and they work

The module on this bench reports `VersionReg = 0x82`. That is not a documented NXP
version. The datasheet lists only 91h (v1.0) and 92h (v2.0), and the library
additionally knows 90h (v0.0) and 88h (Fudan FM17522).

Values outside that set mean a counterfeit die. `miguelbalboa/rfid` issue #428,
"[Collection] Bad boards", collects reports of `0x12`, `0x18`, `0x82`, `0x88`,
`0x8C`, `0xB0`, `0xB2` and `0xB3`, and the consistent theme in that thread is that
these modules **read cards anyway**. Quoted from it: boards reporting `0xB2`
"work absolutely fine with the white credit card tags", and another user found
"the 0xB2 version fails the selftest, but still manages to read the UID most of
the times".

The measured fingerprint on this module makes the counterfeit diagnosis concrete:

| Register | Class | NXP reset | Read | |
|---|---|---|---|---|
| CommandReg | digital | 0x20 | 0x20 | match |
| ComIEnReg | digital | 0x80 | 0x80 | match |
| Status1Reg | digital | 0x21 | 0x21 | match |
| ModeReg | digital | 0x3F | 0x3F | match |
| TxControlReg | digital | 0x80 | 0x80 | match |
| GsNReg | analog | 0x88 | 0x88 | match |
| AutoTestReg | digital | 0x40 | 0x40 | match |
| RxThresholdReg | analog | 0x84 | 0x42 | differs |
| DemodReg | analog | 0x4D | 0x5D | differs |
| RFCfgReg | analog | 0x48 | 0x44 | differs |

Every digital register matched exactly. The three that differ are all receiver
analog front-end configuration. That is the signature of a die with identical
digital logic and a different analog design, which is what a counterfeit is. A
broken bus does not produce seven exact matches and three analog-only misses; it
produces zeroes.

The same module then read a card UID `C7 DA 41 04` with `SAK 0x08` (MIFARE 1K),
returned it identically on five consecutive reads, and detected removal.

**Consequence for the self-test verdicts.** Three checks cannot pass on a
counterfeit module and none of them indicate a fault:

1. `PCD_PerformSelfTest()` switches on the version byte and returns `false` from
   its `default:` case, **before comparing anything**. For any version outside
   `{0x88, 0x90, 0x91, 0x92}` the result carries no information about the chip.
2. The analog register defaults differ, as tabulated above.
3. `RFCfgReg` after `PCD_Init()` does not reach the `0x70` that
   `PCD_SetAntennaGain(RxGain_max)` writes, because clones implement the gain
   field differently. `ModeReg` reaching `0x3D` is the reliable write proof.

So the selftest scores digital registers as proof of a working bus, treats analog
mismatches as advisories, and takes B3/B4/B5 — did it actually read a stable UID
and notice removal — as the acceptance criteria. That matches what this project
needs, since it is read-only by policy and never authenticates, reads memory or
writes.

Known trade-offs of counterfeit modules, from the same issue thread:

- Shorter read range, often 2-3 cm rather than 5 cm.
- Some refuse certain tag types. Blue keyfobs are specifically called out, which
  matters here because the kit includes one. Use the white MIFARE card.
- Write operations are unreliable or absent. Irrelevant to a read-only project.
- One user measured that read performance correlates with antenna impedance
  matching rather than with the chip itself, and that swapping the chip did not
  improve a poorly made board.

A separate report in that thread matters for Phase L: counterfeit modules may
**drive MISO even when their chip select is not asserted**. That corrupts an SPI
loopback test, which is why Phase L requires the reader to be physically removed
rather than merely deselected.

### PCD_Init does not guarantee a hardware reset

```cpp
pinMode(_resetPowerDownPin, INPUT);
if (digitalRead(_resetPowerDownPin) == LOW) {
    ...drive RST low then high, delay 50 ms...
    hardReset = true;
}
if (!hardReset) { PCD_Reset(); }     // soft reset, over SPI
```

Many RC522 modules leave RST floating or fit a pull-up, so that read returns HIGH,
the hardware reset is skipped, and the code falls back to a soft reset issued over
SPI. If SPI is not yet talking, the soft reset achieves nothing and every register
reads back as 0x00. This is the most common RC522 bring-up failure.

The selftest therefore forces RST low then high itself before calling `PCD_Init()`.

`PCD_Init()` also does not call `SPI.begin()`. The sketch must.

The no-argument `PCD_Init()` does honour the chip select and reset pins given to
the constructor, so `MFRC522 rfid(SS_PIN, RST_PIN); rfid.PCD_Init();` is correct.
Only the one-argument `PCD_Init(rstPin)` overload hardcodes `SS`.

### The module is not ready immediately at cold boot

**Measured on this board, 2026-11-18.** The production firmware called
`PCD_Init()` almost immediately after reset and read `VersionReg` as `0x00` — the
value that means nothing is answering on the bus. Seconds later the same module,
with no wiring change, read `0x82` and then read card UIDs perfectly.

The reader was never broken. It was simply not ready that early.

`hw_selftest` never showed this, and the reason is instructive: it runs an 8 second
startup countdown plus several phases of raw SPI probing before it ever calls
`PCD_Init()`, so by then the module has long since settled. A diagnostic that
happens to be slow hides a timing requirement that production code will hit.

**Consequence: initialisation must retry.** `ptRfidBegin()` wraps a single attempt
in a loop of five, 50 ms apart, each performing a full hardware reset rather than
re-reading the same settled state. Every attempt logs its version byte, because a
reader that failed and then worked is exactly the intermittent behaviour nobody
can diagnose later from one line saying it succeeded.

The 60 ms wait after driving RST high covers the datasheet's oscillator start-up
time (crystal start-up plus 37.74 µs) with margin, but that is the *chip's*
start-up. The module's own regulator and the board's 3V3 rail coming up from cold
are separate, and evidently slower.

A related trap: any cached copy of hardware state must be refreshed whenever the
hardware is re-read. The first version of the recovery path set `rfid_ok = true`
but left the cached version byte at the `0x00` from the failed attempt, which
produced the self-contradictory status line `rfid=ok(v0x00)` and reported a value
that had been untrue for minutes.

### Card protocol notes

`STATUS_OK` is the first enumerator, so it equals 0. That makes
`if (!PICC_Select())` an inverted test, which is the bug that broke an earlier
build. Always compare against `STATUS_OK` explicitly.

`PICC_IsNewCardPresent()` sends REQA and already treats `STATUS_COLLISION` as a
card answering, so a collision is a hit and not an error.

Never send a second REQA before SELECT. `PICC_ReadCardSerial()` performs the
SELECT, so it is called exactly once per detection and the card is halted
immediately afterwards.

After `PICC_HaltA()` the card sits in HALT state and stops answering REQA, so
`PICC_IsNewCardPresent()` reports absent while the card is still lying on the
reader. Removal detection has to probe with `PICC_WakeupA()` instead.

**Verified on hardware, 2026-11-18.** The production firmware's probe loop is
WUPA followed by `PICC_HaltA()` on success, and it behaves exactly as the state
machine predicts:

- A card **held** on the reader produces one tap event and then sits in
  `present_watch` indefinitely with no duplicates. This confirms that
  `PICC_IsNewCardPresent()` is not detecting a *new* card at all — reading
  `MFRC522.cpp:1925` shows it sends REQA and never calls `HaltA()`, so what it
  actually detects is a card that is not currently in the ACTIVE state. After
  `PICC_ReadCardSerial()` the card is ACTIVE and answers neither REQA nor WUPA,
  which is what stops the same card re-triggering.
- Removal is reported **only when the card is actually taken away**, never while
  it is held.
- The `HaltA()` after a successful WUPA is load-bearing, not tidy-up. Without it
  the card would be left in READY rather than HALT, so the next probe would be
  testing a different state than the previous one and detection would work once
  and then stop working.

`GetStatusCodeName()` and `PICC_GetTypeName()` return
`const __FlashStringHelper *`, an AVR PROGMEM abstraction. Streaming that through
the bridge-backed Monitor on ARM is not worth betting a bring-up on, so the
selftest maps the enums to plain `const char *` itself. `PCD_DumpVersionToSerial()`
and `PICC_DumpToSerial()` write to `Serial` internally and are avoided for the same
reason.

### Read-only policy

This project never modifies a card. Reading a UID needs only
`PICC_IsNewCardPresent`, `PICC_ReadCardSerial`, `PICC_HaltA` and
`PCD_StopCrypto1`. `tools/no_write_check.sh` enforces that no first-party source
references any mutating function or raw command code.

---

## App Lab layout

| Path | Status |
|---|---|
| `app.yaml` | mandatory manifest |
| `python/main.py` | mandatory entry point |
| `python/requirements.txt` | optional, installed via `uv` |
| `sketch/sketch.ino` | optional MCU code |
| `sketch/sketch.yaml` | mandatory if a sketch exists |
| `assets/` | served by the web_ui brick, must contain `index.html` |
| `data/` | persistent, but excluded from exports unless "include data" is ticked |
| `.cache/` | venv and build cache, excluded from exports |

Apps live at `/home/arduino/ArduinoApps/<AppName>/`. Built-in examples are at
`/var/lib/arduino-app-cli/examples/` and are read-only.

`sketch.yaml` format:

```yaml
profiles:
  default:
    platforms:
      - platform: arduino:zephyr
    libraries:
      - MFRC522 (1.4.12)
default_profile: default
```

Libraries are plain `"Name (version)"` strings. App Lab normally manages this file,
so when using the UI prefer the "Add sketch library" button.

A `tests/` folder is not a documented App Lab convention. Any folder can be added,
but the pytest suite runs on the laptop, not the board.

---

## The .ino purity rule

arduino-cli runs ctags over the `.ino` and inserts a prototype for every function
defined there, hoisted above the rest of the file. A prototype cannot see a type
declared further down, so a merged single-file sketch fails with errors that point
at lines which look perfectly correct:

```
sketch.ino:316:26: error: 'PtVerdict' was not declared in this scope
316 | inline const char* verdictName(PtVerdict v) {
sketch.ino:316:43: error: 'const char* verdictName(PtVerdict)' redeclared as
                          different kind of entity
sketch.ino:1221:31: error: 'MFRC522' has not been declared
```

`g++` cannot reproduce this, because `g++` does not generate prototypes. A laptop
syntax check passes while the board build dies.

The fix is structural. Headers are not processed by ctags, so helpers living in
headers are immune. An `.ino` may define `setup()` and `loop()` and nothing else.
`tools/syntax_check.sh` enforces this.

The same mechanism means a helper in a header that calls another helper defined
below it needs an explicit forward declaration, since headers get no
auto-prototypes either.
