# PhishTap Lite — `hw_selftest`

Bring-up diagnostic for the Arduino UNO Q. Run this **before** the production
sketch. It verifies every peripheral in the order you wire them, and prints a
`PASS` / `FAIL` / `VISUAL` verdict for each.

---

## Files

| File | Purpose |
|---|---|
| `hw_selftest.ino` | Entry point. Phase selection, banner, summary, idle loop. |
| `pt_pins.h` | Single source of truth for every pin. Start here when wiring changes. |
| `pt_log.h` | Monitor logging, float printing, verdict recording, summary table. |
| `pt_font.h` | 5x7 glyph set for the matrix. |
| `pt_matrix.h` | 8x13 matrix wrapper: patterns, text, scrolling. |
| `pt_buzzer.h` | TMB12A05 rhythm engine. All eight game patterns. |
| `phase_a_board.h` | Phase A — matrix + onboard RGB LEDs. 0 wires. |
| `phase_b_rfid.h` | Phase B — RC522 over SPI. 7 wires. Also owns the card-tap confirmation helper. |
| `phase_c_outputs.h` | Phases C1/C2/C3 — lives LEDs and buzzer. |
| `sketch.yaml` | Library manifest (MFRC522 **1.4.12** — see below). |

> ⚠️ **There is deliberately no single-file build.** Merging these headers into
> one `.ino` cannot compile — see *Why the sketch must stay multi-file* below.

---

## Why the sketch must stay multi-file

`arduino-cli` runs ctags over the `.ino` and **inserts a prototype for every
function defined there**, positioned above the rest of the file. A hoisted
prototype cannot see a type that is declared further down, so a merged build
fails on the board with errors that point at lines which look perfectly correct:

```
sketch.ino:316:26: error: 'PtVerdict' was not declared in this scope
316 | inline const char* verdictName(PtVerdict v) {
sketch.ino:316:43: error: 'const char* verdictName(PtVerdict)' redeclared as different kind of entity
sketch.ino:1221:31: error: 'MFRC522' has not been declared
```

`PtVerdict` is declared a few lines above 316, and `MFRC522.h` is included a
thousand lines above 1221 — neither is visible from the hoisted prototype.
`g++` cannot reproduce this, because `g++` does not generate prototypes.

**The fix is structural, not a reorder.** Headers are not processed by ctags, so
helpers living in headers are immune. `hw_selftest.ino` therefore defines
`setup()` and `loop()` and nothing else. `tools/syntax_check.sh` enforces this
and fails any `.ino` that defines another function.

---

## Verification tooling

Three scripts in `tools/`. Run the two gates before every flash.

### `tools/syntax_check.sh` — does it compile?

```bash
tools/syntax_check.sh        # add -v to see the g++ command line
```

Runs `g++ -fsyntax-only` over every `.ino` under `sketch/` using the stub headers
in `tools/stubs/`, then enforces the `.ino` purity rule above. Needs only `gcc`
(`sudo pacman -S gcc`).

It also catches the two bugs it was written after:

- **Missing forward declarations.** Arduino generates prototypes for the `.ino`
  only, never for headers, so `rgbBegin()` calling `rgbAllOff()` before its
  definition is a hard error in a header.
- **Collisions with the core's own symbols.** `Arduino_LED_Matrix.h` declares an
  `extern "C"` global API — `matrixBegin`, `matrixEnd`, `matrixPlay`,
  `matrixSetGrayscaleBits`, `matrixGrayscaleWrite`, `matrixWrite`. Defining our
  own `matrixBegin()` produced
  `error: conflicting declaration of 'void matrixBegin()' with 'C' linkage`.
  Every matrix helper here is therefore prefixed `ptMatrix`.

**What it does not catch.** Anything behavioural. The stubs have no
implementation, so a pass proves the code *compiles*, not that it *works*. Wrong
pin numbers, inverted polarity, bad resistor values and logic errors are all
invisible to it. That is what the on-board phases are for.

### `tools/deploy_to_board.sh` — push over ADB

```bash
tools/deploy_to_board.sh --phases=A          # Phase A only
tools/deploy_to_board.sh --phases=A,B        # add the RC522
tools/deploy_to_board.sh --phases=all
tools/deploy_to_board.sh --dry-run           # show what would happen
```

Pushes `hw_selftest.ino` as `sketch.ino` plus every header into
`/home/arduino/ArduinoApps/hw_selftest/sketch/`, rewriting the `RUN_PHASE_*`
defaults for the phases you asked for. It runs both gates first and **refuses to
deploy** if either fails. ADB works over USB-C with no network at all.

Needs `android-tools` (`sudo pacman -S android-tools`) and the udev rules from
the Stage 5 guide. If `adb` is not on `PATH`, set `ADB=/path/to/adb`.

### `tools/no_write_check.sh` — read-only card policy

```bash
tools/no_write_check.sh        # add --list to see every file scanned
```

Fails if any first-party source references a card-mutating MFRC522 function or
raw command code. Comments are stripped first, so the NO-WRITE policy header can
name every forbidden symbol without tripping the check. `tools/` is excluded,
because `tools/stubs/MFRC522.h` deliberately declares the full upstream API.

### `tools/check_lib.sh` — does the *library* compile?

```bash
tools/check_lib.sh MFRC522 1.4.12            # one version
tools/check_lib.sh MFRC522 1.4.11 1.4.12     # compare versions
tools/deploy_to_board.sh --check-lib ...     # pre-flight whatever sketch.yaml pins
```

Downloads the exact `.zip` that App Lab installs and syntax-checks every `.cpp`
in its `src/`. The GitHub owner is resolved from the Arduino library index, not
guessed.

**Why this matters.** `arduino-cli` compiles *every* `.cpp` in a library's
`src/`, including files the sketch never calls. One broken translation unit fails
the whole build, and the error points into code the project does not own — which
is easy to misread as a wiring or configuration fault.

That is precisely what happened with **MFRC522 1.4.11**, which does not build on
this core:

```
src/MFRC522Extended.cpp:824:34: error: ordered comparison of pointer with
    integer zero ('byte*' {aka 'unsigned char*'} and 'int')
824 |     if (backData && (backLen > 0)) {
```

`backLen` is a `byte *`. Comparing a pointer with `> 0` is an error in C++, not a
warning. Upstream fixed it in **1.4.12** by dereferencing (`*backLen > 0`). This
project never calls `MFRC522Extended` — only the base `MFRC522` class — but the
file is compiled anyway. `sketch.yaml` therefore pins **1.4.12**.

> A stub failure is not always a library bug: it can mean the stubs are missing
> an API the library uses. Compare against a version known to build before
> concluding. Plain-C++ diagnostics such as *ordered comparison of pointer with
> integer zero* are always real.

---

## Flashing

### Option 1 — ADB deploy (recommended)

```bash
cd phishtap-lite
tools/deploy_to_board.sh --phases=A      # or A,B once the RC522 is wired
```

Then press **Run** in App Lab and read the **Serial monitor** tab of the console.
That is the MCU output — see *Reading the console* below.

The app must already exist in App Lab (create it once with **My Apps → Create new
app**, name `hw_selftest`) and the **MFRC522** library must be added with **Add
sketch library**. Use version **1.4.12** — 1.4.11 does not compile on this core
(see `tools/check_lib.sh` above). The library is needed even for Phase A alone:
`phase_b_rfid.h` includes `MFRC522.h` unconditionally.

### Option 2 — App Lab file editor

1. App Lab → **My Apps** → **Create new app**. Name it `hw_selftest`.
2. Paste `hw_selftest.ino` into the sketch pane's `sketch.ino`.
3. Use **Add File** for each header: `pt_pins.h`, `pt_log.h`, `pt_font.h`,
   `pt_matrix.h`, `pt_buzzer.h`, `phase_a_board.h`, `phase_b_rfid.h`,
   `phase_c_outputs.h`.
4. Add the **MFRC522** library with **Add sketch library**.
5. **Run**, then read the **Sketch** console tab.

> Do **not** paste the headers into `sketch.ino` to save steps. See *Why the
> sketch must stay multi-file* above — it will not build.

### Option 3 — `arduino-cli`

The folder is already a valid sketch (`hw_selftest/hw_selftest.ino`), so from the
repo root:

```bash
arduino-cli core update-index
arduino-cli core install arduino:zephyr
arduino-cli lib install MFRC522
arduino-cli compile --fqbn arduino:zephyr:unoq ./sketch/hw_selftest
```

Then upload. On Linux the udev rules must be installed first or the port will not
be accessible — see the Stage 5 guide.

---

## Running the phases

Deploy with the phases you have wired:

```bash
tools/deploy_to_board.sh --phases=A        # 0 wires
tools/deploy_to_board.sh --phases=A,B      # +7 wires (RC522)
tools/deploy_to_board.sh --phases=all      # fully assembled
```

The script rewrites the `RUN_PHASE_*` defaults in a staging copy, so the checked-in
source is untouched. To set them by hand instead, edit the top of
`hw_selftest.ino`:

```c
#define RUN_PHASE_A   1   /* 0 wires            */
#define RUN_PHASE_B   1   /* +7 wires  (RC522)  */
#define RUN_PHASE_C1  0   /* +3 wires  (1 LED)  */
#define RUN_PHASE_C2  0   /* +6 wires  (3 LEDs) */
#define RUN_PHASE_C3  0   /* +5 wires + diode   */
```

Or override at compile time with `arduino-cli`:

```bash
arduino-cli compile --fqbn arduino:zephyr:unoq \
  --build-property "build.extra_flags=-DRUN_PHASE_C1=0 -DRUN_PHASE_C2=0 -DRUN_PHASE_C3=0" \
  ./sketch/hw_selftest
```

**Recommended order:** A → B → C1 → C2 → C3. Phase B before Phase C on purpose —
the RC522 is the only component whose failure would force a redesign.

---

## Reading the console

App Lab's console has **three tabs**, and only one of them shows the sketch:

| Tab | Shows | Is it the sketch? |
|---|---|---|
| **App Launch** | container start, provisioning, `uv`/venv messages | no |
| **Python** | the Linux side — `python/main.py`, `Hello world!`, `App started` | no |
| **Serial monitor** | **the STM32 MCU** — selftest banner, PHASE A/B, SUMMARY | **yes** |

The Serial monitor tab shows a fixed **9600 baud** that cannot be changed. That is
harmless. On this board `Serial` and `Monitor` are literally the same object —
`variants/arduino_uno_q_stm32u585xx/variant.h` defines
`ARDUINO_ROUTERBRIDGE_PROVIDES_SERIAL`, and `Arduino_RouterBridge`'s
`singletons.cpp` declares:

```cpp
extern BridgeMonitor<> Serial [[gnu::alias("Monitor")]];
```

`BridgeMonitor::begin()` also explicitly discards its baud argument
(`(void)_legacy_baud;`). The channel is virtual and carried over the router, so
the baud rate is cosmetic.

> ⚠️ **Open the Serial monitor tab BEFORE pressing Run.** The selftest emits most
> of its output from `setup()`, within the first seconds. Anything printed before
> the tab is connected is lost, which looks identical to "the sketch printed
> nothing".
>
> If you missed it, re-deploy with `--loop` so the whole run repeats forever:
> ```bash
> tools/deploy_to_board.sh --phases=A,B --loop
> ```
> A non-looping build also waits `PT_STARTUP_COUNTDOWN_S` (default 8 s) with a
> visible countdown on the matrix before starting, which is usually enough time.

---

## Wiring

> ⚠️ **RC522 VCC goes to 3V3. Never 5 V.** The chip is a 3.3 V part and 5 V
> destroys it.
>
> ⚠️ **Read the module's silkscreen.** RC522 boards label the chip select either
> `SS` or `SDA`, and the pin order varies between sellers. Match the label, not
> the position.

### Phase A — 0 wires

Nothing to connect. Power the board and watch.

### Phase B — RC522, 7 wires

| RC522 pin | UNO Q pin |
|---|---|
| VCC / 3.3V | **3V3** |
| GND | **GND** |
| RST | **D5** |
| SDA / SS | **D10** |
| SCK | **D13** |
| MOSI | **D11** |
| MISO | **D12** |
| IRQ | not connected |
| EA | not connected |

### Phase C1 / C2 — lives LEDs

For each LED: `pin → 150 Ω → LED anode (long leg) → LED cathode (short leg) → GND`.

| LED | Pin |
|---|---|
| Life 1 | **D7** |
| Life 2 | **D8** |
| Life 3 | **D9** |

These are **active-high** — the pin sources current. That is the opposite of the
onboard RGB LEDs.

Resistor value depends on LED colour:

| Colour | Vf | Resistor | Current |
|---|---|---|---|
| Red / amber | ~2.0 V | **150 Ω** | 8.7 mA |
| Green | ~2.1 V | **150 Ω** | 8.0 mA |
| Blue / white | ~3.0 V | **68 Ω** | 4.4 mA |

`R = (3.3 − Vf) / I`. Keep `I` at 10 mA or below per pin.

### Phase C3 — buzzer (TMB12A05, active)

| # | From | To |
|---|---|---|
| 1 | **D6** | 1 kΩ → transistor **BASE** |
| 2 | transistor **BASE** | 10 kΩ → **GND** |
| 3 | transistor **EMITTER** | **GND** |
| 4 | transistor **COLLECTOR** | buzzer **−** |
| 5 | buzzer **+** | **5V pin** |
| 6 | 1N4007 **CATHODE** (silver stripe) | buzzer **+** |
| 7 | 1N4007 **ANODE** | buzzer **−** |

**Transistor pinout trap.** TO-92 flats differ by part *and* by manufacturer.
With the flat face toward you, left to right:

| Marking on the body | Pin order |
|---|---|
| `PN2222A` | **E – B – C** |
| `P2N2222A` | **C – B – E** |
| `BC547` | **C – B – E** |

The base is the middle pin in all three, but emitter and collector are swapped.
Read the marking on *your* transistor, look up *that* marking, and confirm the
base with a multimeter diode test **before** powering on. A hot transistor means
it is backwards — power off immediately.

**Buzzer polarity.** The TMB12A05 case is marked `+`. That leg goes to the 5 V
rail. Reversed, it stays silent.

**No series resistor.** The part needs at least 4.0 V across it. At ~20 mA, a
47 Ω resistor would drop ~0.95 V and, together with the transistor's saturation
drop, could put the buzzer below its minimum operating voltage. If it is too
loud, shorten the beeps or damp the sound hole — do not add resistance without
first measuring the voltage across the buzzer with a multimeter.

---

## Confirming the visual and audible phases

The matrix, the LEDs and the buzzer have no readback, so the firmware cannot tell
whether they worked. Those rows are reported as `VISUAL`.

Once Phase B has proven the reader, later phases ask you to confirm by
**tapping the card**:

- Tap within the timeout → recorded as `PASS`
- Let it time out → recorded as `FAIL`

Monitor keyboard input is deliberately not used. Whether the virtual serial
channel carries keystrokes back to the MCU is unverified, and a selftest that
hangs waiting for input that can never arrive is worse than one that asks you to
tap a card.

After the run finishes, tapping a card replays all eight buzzer patterns so you
can listen again without reflashing.

---

## Expected output

```
##############################################
# PhishTap Lite - HARDWARE SELFTEST          #
##############################################
version 1.0.0
phases enabled: A=1 B=1 C1=1 C2=1 C3=1

==============================================
PHASE A - BOARD ONLY (0 wires)
==============================================
A1: Monitor logging
    board F_CPU=160000000 Hz, millis=42
    RGB constants resolved via named macros (LED3_R..LED4_B)
>>> [A1-log] PASS - Monitor output reached you
...
>>> [A2-matrix] VISUAL - Confirm all 6 patterns by eye; panel has no readback
>>> [A3-rgb] VISUAL - Confirm colours and ACTIVE-LOW polarity by eye

==============================================
PHASE B - RC522 RFID READER (7 wires)
==============================================
    SPI clock in use: 1000000 Hz
B1: VersionReg = 0x91 -> MFRC522 v1.0
>>> [B1-spi] PASS - MFRC522 v1.0
B2: PCD_PerformSelfTest() = true
>>> [B2-selftest] PASS - Chip internal self-test passed

B3: PLACE A CARD on the reader now (30s timeout)
    UID (4 bytes): A1 B2 C3 D4
    SAK: 0x08 -> MIFARE 1K
    Time to first read: 812.3 ms
>>> [B3-uid] PASS - A1 B2 C3 D4
...
    5/5 reads succeeded, UID consistent
>>> [B4-stable] PASS - 5/5 identical reads
>>> [B5-removal] PASS - Removal detected via WUPA timeout

================= SUMMARY ==================
  A1-log  PASS    Monitor output reached you
  A2-matrix VISUAL  Confirm all 6 patterns by eye; panel has no readback
  ...
=============================================
No hard failures.
2 row(s) marked VISUAL need your eyes or ears - see the log above.
```

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `error: 'PtVerdict' was not declared in this scope` (or `PtBuzzPattern`, `MFRC522`) | The headers were merged into `sketch.ino`. arduino-cli hoisted a prototype above the type declaration | Use the multi-file layout. `tools/syntax_check.sh` enforces this |
| `error: 'verdictName' redeclared as different kind of entity` | Same cause — the hoisted prototype and the real definition disagree | Same fix |
| `error: conflicting declaration of 'void matrixBegin()' with 'C' linkage` | A helper collides with the core's `extern "C"` matrix API | Prefix it `ptMatrix…`. Reserved names: `matrixBegin`, `matrixEnd`, `matrixPlay`, `matrixSetGrayscaleBits`, `matrixGrayscaleWrite`, `matrixWrite`, `reverse` |
| `error: ordered comparison of pointer with integer zero` in `MFRC522Extended.cpp` | MFRC522 **1.4.11** is pinned | Bump `sketch.yaml` to `MFRC522 (1.4.12)`. Verify with `tools/check_lib.sh MFRC522 1.4.12` |
| `warning: library MFRC522 claims to run on avr, megaavr, … may be incompatible … zephyr` | `library.properties` predates the Zephyr core | **Harmless.** The library is plain C++/SPI and builds fine; only a warning |
| `fatal error: MFRC522.h: No such file or directory` | Library not added to the sketch | App Lab → **Add sketch library** → MFRC522 (GithubCommunity) **1.4.12** |
| No console output at all | `arduino-router` not running | `systemctl status arduino-router`, then `sudo systemctl restart arduino-router` |
| No console output, router fine | Wrong console tab | Read the **Serial monitor** tab, not Python or App Launch |
| Serial monitor empty even though the matrix animates | The tab was opened after `setup()` already printed | Deploy with `--loop` so the run repeats, or open the tab before pressing Run |
| `VersionReg = 0x00` | MISO stuck low, or no power | Check GND, check 3V3, check MISO is on D12 |
| `VersionReg = 0xFF` | MISO floating | Check MISO wiring and solder joints on the module header |
| Version OK but no card detected | 125 kHz card, or antenna | An RC522 **cannot** read 125 kHz. Test the card with an Android phone's NFC reader |
| Version OK, card detected, unstable reads | Marginal SPI or noisy antenna | Shorten jumpers below 20 cm; keep them away from the antenna coil |
| Self-test fails, version `0x88` | FM17522 clone | Expected. Clones fail the NXP reference comparison and still read cards fine |
| LED never lights | Reversed LED, or wrong resistor | Long leg is the anode. Blue/white needs 68 Ω, not 150 Ω |
| Buzzer silent | Transistor backwards, buzzer reversed, diode reversed, no 5 V | Work through the C3 checklist in the log output |
| Buzzer silent, transistor hot | Emitter and collector swapped | **Power off immediately.** Check the pinout for your exact part marking |
| Buzzer quiet | Undervolt | Measure across the buzzer while sounding; it must be ≥ 4.0 V |

---

## After the selftest

Once every phase is `PASS` or a confirmed `VISUAL`, report the console output back
verbatim. Stage 1b (the production sketch) depends on those results — in
particular on the RGB polarity, the `VersionReg` value, and whether the UID is
4 bytes.
