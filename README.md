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
tools/                 verification gates and deploy scripts
```

Later stages add `python/`, `assets/`, `tests/` and the production sketch. The
planned tree is in ## Start here

Read `docs/HARDWARE.md` before wiring anything. It has the pin map, the resistor
maths, the transistor pinout trap, and the two library version pins that are easy
to get wrong.

Then `sketch/hw_selftest/README.md` for the bring-up procedure. It runs in phases
so a failure points at one thing rather than everything.

## Verification

Three gates run before anything reaches the board:

```bash
tools/syntax_check.sh      # g++ -fsyntax-only against stub headers, plus the .ino purity rule
tools/no_write_check.sh    # no card-mutating MFRC522 call anywhere in first-party code
tools/check_lib.sh MFRC522 1.4.12   # does the pinned library itself compile
```

And two for working with the board:

```bash
tools/deploy_to_board.sh --phases=A,B   # push over adb, gates run first
tools/board_doctor.sh                   # what is actually on the board right now
```

## Status

Stage 1a is done: the hardware selftest covers the matrix, RGB LEDs, RC522, lives
LEDs and buzzer, and Phase A passes on real hardware. Phase B is in progress.

Stage 1b onwards is not started. See the stage list at the end of
