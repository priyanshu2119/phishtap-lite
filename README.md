# PhishTap Lite

A tabletop arcade console that trains students to spot phishing.

A student taps an RFID card on an RC522 reader. A QR code appears on a laptop
screen. Scanning it opens a mobile web game that shows one message at a time,
styled as an email, SMS or WhatsApp chat. The student answers PHISH or SAFE before
a timer that shrinks every round. A wrong answer or a timeout costs one of three
lives. When the game ends the console shows the score and the phone shows a review
page explaining what was wrong in every message.

Built for a college demo, entirely offline, on a single Arduino UNO Q.

## How it works

```
student taps card
      |
      v
STM32U585 (MCU)  -- reads the 4-byte UID over SPI, queues an event
      |
      | internal UART, MessagePack-RPC via arduino-router
      v
QRB2210 (MPU)    -- creates a session, token + 4-digit PIN, 90s TTL
      |
      | Socket.IO
      v
laptop browser   -- kiosk page renders the QR client-side
      |
      | phone camera scans it
      v
student's phone  -- the game: one message, PHISH or SAFE, shrinking timer
```

The MCU owns everything real-time. Python owns game logic, storage and the web
server. Neither reaches into the other's job.

## Repository layout

```
docs/                  architecture and verified hardware notes
sketch/hw_selftest/    bring-up diagnostic for the board and peripherals
sketch/phishtap/       production firmware (Stage 1b)
tools/                 verification gates, host unit tests and deploy scripts
```

Later stages add `python/`, `assets/` and `tests/`. The planned tree is in
## Start here

Read `docs/HARDWARE.md` before wiring anything. It has the pin map, the resistor
maths, the transistor pinout trap, the two library version pins that are easy to
get wrong, and the counterfeit-module analysis that explains why a reader
reporting version `0x82` is fine.

Then `sketch/hw_selftest/README.md` for the bring-up procedure. It runs in phases
so a failure points at one thing rather than everything.

For the firmware itself, `sketch/phishtap/README.md`.

## Verification

Five gates, in increasing strength:

```bash
tools/syntax_check.sh      # g++ against stub headers, -Wall -Wextra, .ino purity rule
tools/no_write_check.sh    # no card-mutating MFRC522 call anywhere in first-party code
tools/host_gate.sh         # 327 assertions on the Arduino-free modules, ASan + UBSan
tools/check_lib.sh MFRC522 1.4.12   # does the pinned library itself compile
tools/board_compile.sh --sketch phishtap   # the REAL Zephyr toolchain, on the board
```

`host_gate.sh` is worth understanding. `pt_json.h` and `pt_events.h` are written
with no Arduino dependency, so the actual production code runs under g++ with
assertions before it is ever flashed. It also pipes a payload written by the
firmware's own writer through Python's `json` module, an independent RFC 8259
parser. A codec that only ever validates itself proves nothing.

`board_compile.sh` closes the other gap: `syntax_check.sh` compiles against
hand-written stubs, which are only as good as whoever wrote them, so this compiles
against the real core and the real libraries. It does not flash or run anything.

And two for working with the board:

```bash
tools/deploy_to_board.sh --sketch phishtap  # push over adb, all gates run first
tools/run_phase.sh diag                     # RC522 investigation on hw_selftest
tools/board_doctor.sh                       # what is actually on the board right now
```

## Status

**Stage 1a is done.** The hardware selftest covers the matrix, RGB LEDs, RC522,
lives LEDs and buzzer. Phases A, L, W and B all pass on real hardware: the reader
returned UID `C7 DA 41 04`, SAK `0x08` (MIFARE 1K), five identical reads, and
detected removal.

The module is a counterfeit die reporting version `0x82`. That is normal, it reads
cards fine, and the two warnings it produces are unavoidable on any clone. Read the
