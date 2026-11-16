# PhishTap Lite — `hw_selftest`

Bring-up diagnostic for the Arduino UNO Q. Run this **before** the production
sketch. It verifies every peripheral in the order you wire them, and prints a
`PASS` / `FAIL` / `VISUAL` verdict for each.

---

## Files

| File | Purpose |
|---|---|
| `hw_selftest.ino` | Entry point. Phase selection, banner, summary, idle loop. Defines `setup()` and `loop()` and nothing else. |
| `pt_pins.h` | Single source of truth for every pin. Start here when wiring changes. |
| `pt_log.h` | Monitor logging, float printing, verdict recording, summary table. |
| `pt_font.h` | 5x7 glyph set for the matrix. Required, because `ArduinoGraphics` is not bundled with this core. |
| `pt_matrix.h` | 8x13 matrix wrapper: patterns, text, scrolling. All names prefixed `ptMatrix` to avoid the core's `extern "C"` globals. |
| `pt_prompt.h` | Operator prompting for tests that need a jumper moved. Console plus matrix, card tap or countdown. |
| `pt_buzzer.h` | TMB12A05 rhythm engine. All eight game patterns. |
| `pt_spi_probe.h` | Raw SPI access shared by Phases L, W and B: hardware reset, register reads, clock sweep, and the datasheet reset-value fingerprint. |
| `phase_a_board.h` | Phase A — matrix + onboard RGB LEDs. 0 wires. |
| `phase_l_loopback.h` | Phase L — SPI loopback. RC522 **removed**, one D11→D12 jumper. |
| `phase_w_linestate.h` | Phase W — MISO line state. RC522 wired. |
| `phase_b_rfid.h` | Phase B — full RC522 test. 7 wires. Also owns the card-tap confirmation helper. |
| `phase_c_outputs.h` | Phases C1/C2/C3 — lives LEDs and buzzer. |
| `pt_runner.h` | Phase dispatch and the startup countdown. Included last. |
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

Four scripts in `tools/`. Run the two gates before every flash.

### `tools/run_phase.sh` — run one diagnostic phase

```bash
tools/run_phase.sh L          # SPI loopback, RC522 removed
tools/run_phase.sh W          # MISO line state, RC522 wired
tools/run_phase.sh diag       # L then W then B
tools/run_phase.sh L --no-deploy   # print the guidance, touch nothing
```

Prints the wiring each phase needs and the decision table for reading its result
*before* deploying, so the console output can be interpreted as it arrives rather
than decoded afterwards. Supports `--loop` and `--app`.

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

### Option 1 — `run_phase.sh` (recommended)

Prints the wiring and the decision table for the phase you asked for, then
deploys it:

```bash
cd phishtap-lite
tools/run_phase.sh A          # board only, no wiring
tools/run_phase.sh L          # SPI loopback, RC522 REMOVED, D11-D12 jumper
tools/run_phase.sh W          # MISO line state, RC522 wired
tools/run_phase.sh B          # full RC522 test
tools/run_phase.sh C          # lives LEDs and buzzer
tools/run_phase.sh diag       # L then W then B, the full RC522 investigation
tools/run_phase.sh L --no-deploy   # just show me the guidance
```

Add `--loop` to any of them so the run repeats forever, which means the Serial
monitor tab can be opened after Run was pressed without missing the output.

### Option 2 — `deploy_to_board.sh` directly

```bash
tools/deploy_to_board.sh --phases=A          # or A,B  or L  or L,W,B
tools/deploy_to_board.sh --phases=A,B --loop --check-lib
```

Then press **Run** in App Lab and read the **Serial monitor** tab of the console
(called **Sketch** in some App Lab versions). That is the MCU output — see
*Reading the console* below.

The app must already exist in App Lab (create it once with **My Apps → Create new
app**, name `hw_selftest`) and the **MFRC522** library must be added with **Add
sketch library**. Use version **1.4.12** — 1.4.11 does not compile on this core
(see `tools/check_lib.sh` above). The library is needed even for Phase A alone:
`phase_b_rfid.h` includes `MFRC522.h` unconditionally.

### Option 3 — App Lab file editor

1. App Lab → **My Apps** → **Create new app**. Name it `hw_selftest`.
2. Paste `hw_selftest.ino` into the sketch pane's `sketch.ino`.
3. Use **Add File** for each header: `pt_pins.h`, `pt_log.h`, `pt_font.h`,
   `pt_matrix.h`, `pt_prompt.h`, `pt_buzzer.h`, `pt_spi_probe.h`,
   `phase_a_board.h`, `phase_l_loopback.h`, `phase_w_linestate.h`,
   `phase_b_rfid.h`, `phase_c_outputs.h`, `pt_runner.h`.
4. Add the **MFRC522** library with **Add sketch library**, version 1.4.12.
5. **Run**, then read the **Serial monitor** console tab (called **Sketch** in
   some App Lab versions).

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

`tools/run_phase.sh` is the easy way in. It prints the wiring and the decision
table for the phase you asked for, then deploys it:

```bash
tools/run_phase.sh A          # board only, 0 wires
tools/run_phase.sh L          # SPI loopback, RC522 REMOVED, D11-D12 jumper
tools/run_phase.sh W          # MISO line state, RC522 wired
tools/run_phase.sh B          # full RC522 test, RC522 wired
tools/run_phase.sh C          # lives LEDs and buzzer
tools/run_phase.sh diag       # L then W then B
tools/run_phase.sh L --no-deploy    # guidance only, do not touch the board
```

Or drive the deploy script directly:

```bash
tools/deploy_to_board.sh --phases=A
tools/deploy_to_board.sh --phases=L
tools/deploy_to_board.sh --phases=A,B
tools/deploy_to_board.sh --phases=all
tools/deploy_to_board.sh --phases=A,B --loop --check-lib
```

Both rewrite the `RUN_PHASE_*` defaults in a staging copy, so the checked-in
source is untouched. To set them by hand instead, edit the top of
`hw_selftest.ino`:

```c
#define RUN_PHASE_A   1   /* 0 wires                        */
#define RUN_PHASE_L   0   /* RC522 REMOVED, D11-D12 jumper  */
#define RUN_PHASE_W   0   /* +7 wires  (RC522, line state)  */
#define RUN_PHASE_B   1   /* +7 wires  (RC522, full test)   */
#define RUN_PHASE_C1  0   /* +3 wires  (1 LED)              */
#define RUN_PHASE_C2  0   /* +6 wires  (3 LEDs)             */
#define RUN_PHASE_C3  0   /* +5 wires + diode (buzzer)      */
```

Or override at compile time with `arduino-cli`:

```bash
arduino-cli compile --fqbn arduino:zephyr:unoq \
  --build-property "build.extra_flags=-DRUN_PHASE_L=1 -DRUN_PHASE_B=0" \
  ./sketch/hw_selftest
```

### Recommended order

**First bring-up:** A → B → C1 → C2 → C3.

Phase B runs before Phase C on purpose. The RC522 is the only component whose
failure would force a redesign, so it is worth knowing about that before spending
time on LEDs and a buzzer.

**If Phase B reports `0x00` at every clock**, switch to the diagnostic order:

1. **Step 0** — the LED power test above. Two minutes, no code, and it proves
   which physical header pins are really 3V3 and GND.
2. **Phase L** — RC522 removed, one D11→D12 jumper. Proves the board's SPI works
   with no slave involved. If this fails, the RC522 is irrelevant.
3. **Phase W** — RC522 wired. Proves the module is powered and driving MISO,
   without needing any SPI traffic to succeed.
4. **Phase B** again — with the electrical layer already settled, a failure here
   is a protocol problem and B0c/B1b say which one.

`tools/run_phase.sh diag` runs steps 2 to 4 in one flash. It will ask you to change
the wiring twice; each change is announced on the console and scrolled on the
matrix, then a countdown runs. The console cannot take keystrokes, so the countdown
is the only way to tell the board you are ready.

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

> ⚠️ **RC522 VCC goes to 3V3. Never 5 V.** The chip is a 2.5–3.3 V part and 5 V
> destroys it.
>
> ⚠️ **Read the module's silkscreen.** RC522 boards label the chip select either
> `SS` or `SDA`, and the pin order varies between sellers. Match the label, not
> the position.
>
> ⚠️ **JANALOG is crowded and unforgiving.** Do Step 0 below before connecting the
> RC522.

### Step 0 — prove which header pins are 3V3 and GND (do this first)

The JANALOG header order, from the datasheet:

| Pin | Name | What it is |
|---|---|---|
| 1 | **BOOT** | MCU_BOOT0. Jumper here and the MCU enters bootloader mode. |
| 2 | **IOREF** | 3.3 V, **output only**. Reference voltage, not a supply. |
| 3 | **RESET** | MCU_NRST. Jumper here and the board reboots. |
| 4 | **+3V3** | 3.3 V out. **This is the one the RC522 needs.** |
| 5 | **+5V** | 5V_SYS. **Feeds 5 V into a 2.5–3.3 V part and kills it.** |
| 6 | **GND** | |
| 7 | **GND** | |
| 8 | **VIN** | 7–24 V **input**. Never an output. |
| 9+ | A0–A5 | ADC. Not 5 V tolerant. |

The 3V3 pin sits **between RESET and +5V**. One pin off in either direction either
reboots the MCU, puts it in bootloader mode, or destroys the RC522. Counting
positions is how that mistake gets made, so identify the pins electrically instead.

**No multimeter needed.** You need one LED and one 150 Ω resistor.

1. Power the board. It can stay connected to the laptop or the charger.
2. Build the test lead: `LED anode (long leg) → 150 Ω → free jumper`. A second
   jumper goes from `LED cathode (short leg)`.
3. Put the cathode jumper on a pin you are confident is **GND** (pin 6 or 7).
4. Touch the anode-side jumper to the pin you believe is **+3V3** (pin 4).

| Result | Meaning |
|---|---|
| **LED lights** | That pin is 3V3 and your GND is real. Mark both with tape. |
| LED does not light | Either pin is wrong. Try the other GND (pin 6 vs 7). |
| LED lights very brightly, or the board reboots | You are on **+5V**, not 3V3. Stop. |
| Board reboots when you touch a pin | That pin is **RESET**. Move one pin over. |
| Nothing happens at all, board seems dead | That pin may be **BOOT**. Power cycle. |

5. Repeat with the anode-side jumper on the pin you believe is **+5V** (pin 5).
   The LED should light **noticeably brighter** than on 3V3, because
   `(5 − 2) / 150 ≈ 20 mA` versus `(3.3 − 2) / 150 ≈ 8.7 mA`. That brightness
   difference is how you tell the two rails apart with no meter.

> Do not leave the LED on the 5V pin. 20 mA is fine briefly, but there is no
> reason to leave it there.

6. **Only now** connect the RC522, using the pins you just proved.

If you have a multimeter, measure between the pin and GND instead: 3.3 V ± 0.2 for
pin 4, 5.0 V ± 0.25 for pin 5. The 3.3 V rail minimum is 3.1 V; below that the
board itself browns out.

Note that **+3V3 also appears on JMISC and JMEDIA**. Do not use those headers.
JMEDIA and JMISC carry 1.8 V processor I/O with a 2.1 V absolute maximum, and
JCTL is the 1.8 V debug UART. None of them should ever see a 3.3 V signal.

### Step 0b — photos to send before asking for wiring help

A wiring fault is far easier to spot in a picture than to describe in text. Take
these two photos and send them:

**Photo 1 — the RC522 module, front side, wires attached.**
- The **silkscreen labels must be legible**. That is the whole point: the pin order
  varies between sellers, so the labels are the only reliable reference.
- Show each jumper leaving the board, ideally with a finger or a label pointing at
  which silkscreen pin it is on.
- Include the whole module so the antenna coil and any soldered header are visible.

**Photo 2 — the UNO Q header, close up, wires attached.**
- Show the **silkscreen pin names next to the board**, not just the wires.
- Angle it so the JANALOG pin order (BOOT, IOREF, RESET, +3V3, +5V, GND, GND, VIN)
  can be read, since that is where a one-pin-off error happens.
- If the jumpers hide the silkscreen, take one photo with them in place and one
  lifted aside.

**Typical RC522 module pin order** (one common arrangement, top to bottom):

```
SDA  SCK  MOSI  MISO  IRQ  GND  RST  3.3V
```

> ⚠️ **This is only typical.** Other sellers ship `3.3V RST GND IRQ MISO MOSI SCK
> SDA`, or put the header along a different edge, or label chip select `SS`
> instead of `SDA`. **The silkscreen on your module wins over any table, including
> this one.** That is why Photo 1 needs the labels legible.

Also check, before photographing: **are the module's header pins actually
soldered?** Cheap RC522 boards frequently ship with the header loose in the bag.
A pin that is merely pressed into the hole looks connected and is not. Look for a
shiny solder fillet around each pin on the underside.

### Phase A — 0 wires

Nothing to connect. Power the board and watch.

### Phase L — SPI loopback, 1 wire, RC522 REMOVED

**Remove all seven RC522 wires first.** The module must be completely disconnected,
because the point of this phase is to prove the board's SPI works with no slave
involved.

Then fit exactly one jumper:

| From | To |
|---|---|
| **D11** (MOSI) | **D12** (MISO) |

Nothing else. No resistor, no other connection.

The sketch echoes fifteen patterns back through that jumper at 100 kHz, 1 MHz and
4 MHz, three times each, and prints a sent/received table. Whatever the peripheral
shifts out must come back in, so a mismatch is the board's fault and not the
module's.

> Remember to **remove this jumper** before Phase W or Phase B. Leaving it fitted
> shorts MOSI to MISO and the RC522 will never work.

### Phase W — MISO line state, RC522 wired

Same seven wires as Phase B below, and **no loopback jumper**. This phase does not
clock the bus at all; it holds chip select at each level in turn and reads D12 as a
plain input, so the module's own drive strength is what gets measured.

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

Do **Step 0** above before trusting this table against your own board. The 3V3 pin
sits between RESET and +5V on JANALOG, and one pin off in either direction reboots
the MCU or destroys the module.

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
| `VersionReg = 0x00` at **every** clock (100 kHz, 1 MHz, 4 MHz) | MISO held low for the whole transfer. Speed-independence rules out signal integrity | Run **Step 0** (LED power test), then **Phase L**, then **Phase W**. See the table below |
| `VersionReg = 0xFF` at every clock | MISO floating or held high | Check MISO is really in D12, and look for a jumper on **IOREF** (a 3V3 output two pins from 3V3) |
| B0a fingerprint: **all ten** registers read `0x00` | Bus dead or module unpowered | Phase W will tell you which. An unpowered RC522 clamps MISO low through its ESD diode, which looks exactly like a short to ground |
| B0a fingerprint: **some** registers match | Bus works, bits are corrupting | Compare the expected and actual columns. If the differences are all in one bit position, suspect one specific data line |
| B0b: 20 reads all identical but wrong | Line is **stuck**, not noisy | A wiring or power fault, not a contact fault. Do not bother re-seating jumpers first |
| B0b: 20 reads vary | **Intermittent** contact or noise | Re-seat every jumper, keep them under 20 cm, route away from the antenna coil |
| B0c: manual SS and library SS disagree | Chip select handling is the fault | The overlay declares `st,gpio-control`. Whichever value is a plausible register default is the path that actually selected the chip |
| B1b: registers still at reset values after `PCD_Init()` | Reads work, **writes do not** | Suspect MOSI. It may be swapped with MISO, or not making contact. Reads still succeed because MISO is fine |
| Phase L: no echo at any clock, with the D11→D12 jumper fitted | SPI not driving these pins | Confirm the jumper is really in D11 and D12, then investigate pinctrl, `zephyr,deferred-init`, and whether the `SPI` object bound to SPI2 |
| Phase L: repeats disagree with each other | Intermittent contact on the loopback jumper | Re-seat it and re-run before concluding anything |
| Phase W: MISO low even with pull-up, in both SS states | Module **unpowered**, or MISO shorted to GND | Do Step 0. JANALOG order is BOOT, IOREF, RESET, +3V3, +5V, GND, GND, VIN |
| Phase W: MISO follows the pull in **both** SS states | Nothing is ever driving MISO | The MISO wire is not making contact, MOSI/MISO are swapped, or the module header is unsoldered |
| Board reboots when you touch a header pin with a jumper | That pin is **RESET** | JANALOG pin 3. Move one pin over to reach +3V3 |
| Board stops responding, matrix dark, after wiring | Jumper landed on **BOOT** | JANALOG pin 1. Remove it and power cycle |
| RC522 gets hot, or died after wiring | **5 V was applied to VCC** | JANALOG pin 5 is +5V, one pin from +3V3. The module is a 2.5–3.3 V part and is now likely destroyed |
| Version OK but no card detected | 125 kHz card, or antenna | An RC522 **cannot** read 125 kHz. Test the card with an Android phone's NFC reader |
| Version OK, card detected, unstable reads | Marginal SPI or noisy antenna | Shorten jumpers below 20 cm; keep them away from the antenna coil |
| Self-test fails, version `0x88` | FM17522 clone | Expected. Clones fail the NXP reference comparison and still read cards fine |
| LED never lights | Reversed LED, or wrong resistor | Long leg is the anode. Blue/white needs 68 Ω, not 150 Ω |
| Buzzer silent | Transistor backwards, buzzer reversed, diode reversed, no 5 V | Work through the C3 checklist in the log output |
| Buzzer silent, transistor hot | Emitter and collector swapped | **Power off immediately.** Check the pinout for your exact part marking |
| Buzzer quiet | Undervolt | Measure across the buzzer while sounding; it must be ≥ 4.0 V |

### Diagnosing `0x00` at every clock

This is the failure the diagnostic phases exist for. Work down the table; each row
eliminates a family of causes.

| Phase L result | Phase W result | Conclusion |
|---|---|---|
| echo OK | drives low when selected | Board and wiring are both fine. The fault is at the protocol layer. Study B0c and B1b |
| echo OK | low even with pull-up | Board is fine. The **module is not powered**, or MISO is shorted to GND, or the module is dead |
| echo OK | follows the pull in both states | Board is fine. The **MISO wire is not connected**, or MOSI/MISO are swapped |
| no echo | any | The **board's SPI is not reaching these pins**. The RC522 is irrelevant until this is fixed |

---

## After the selftest

Once every phase is `PASS` or a confirmed `VISUAL`, report the console output back
verbatim. Stage 1b (the production sketch) depends on those results — in
particular on the RGB polarity, the `VersionReg` value, and whether the UID is
4 bytes.
