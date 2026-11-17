# PhishTap Lite — Architecture

A tabletop arcade console that trains students to spot phishing. A student taps an
RFID card, a QR code appears on a laptop screen, scanning it opens a mobile web
game. The game shows one message at a time (email, SMS, WhatsApp) and the student
answers PHISH or SAFE before a timer that shrinks every round. A wrong answer or a
timeout costs a life. When the game ends the console shows the score and the phone
shows a review page explaining what was wrong in every message.

Target hardware: Arduino UNO Q (4 GB / 32 GB), one board, no ESP32.

---

## 1. Platform

The UNO Q carries two processors on a single PCB. They talk over an internal UART
at 115200 baud, mediated by the `arduino-router` systemd service using
MessagePack-RPC.

| | MCU | MPU |
|---|---|---|
| Chip | STM32U585, Cortex-M33 @ 160 MHz | Qualcomm QRB2210, 4x Cortex-A53 @ 2.0 GHz |
| Memory | 2 MB flash, 786 KB SRAM | 32 GB eMMC, 4 GB LPDDR4 |
| OS | Zephyr RTOS | Debian (Trixie), Python 3.13 |
| Owns | RC522 polling, countdown, matrix, LEDs, buzzer | game logic, SQLite, web server, analytics |
| Bridge side | `Arduino_RouterBridge` 0.4.4 | `arduino.app_utils` |

The MCU has no USB and no Wi-Fi of its own. Uploads, serial output and networking
all pass through the Linux side.

`/dev/ttyHS1` is reserved by the router service and must never be opened directly.
`Serial1` on D0/D1 is free, but this project leaves it untouched.

### Division of labour

The MCU owns everything real-time: the non-blocking RC522 state machine with
removal detection and a 3 s same-card cooldown, the per-round countdown, matrix
animations, lives LEDs, the buzzer rhythm engine, RGB status LEDs, and later
button capture with `micros()` timestamps. Python never animates frame by frame.

Python owns game logic, scoring, sessions, SQLite, analytics, the web UI, QR/PIN
pairing and admin.

### Layering rule

The game engine, DB and API handlers are pure Python modules with no App Lab
imports. Only two thin adapters touch `arduino.*`. This is enforced by test, and
it is what makes the laptop-only dev mode possible: a mock bridge plus a local web
server serving the same assets.

---

## 2. Pin map

Verified against the official UNO Q pin tables.

| Pin | STM32 | Function | Notes |
|---|---|---|---|
| D10 | PB9 | RC522 SS | also PWM-capable, unused here |
| D11 | PB15 | RC522 MOSI | also PWM-capable, unused here |
| D12 | PB14 | RC522 MISO | |
| D13 | PB13 | RC522 SCK | |
| D5 | PA11 | RC522 RST | also FDCAN1_RX, unused |
| D7 | PB2 | life LED 1 | plain GPIO |
| D8 | PB4 | life LED 2 | plain GPIO |
| D9 | PB8 | life LED 3 | also PWM-capable, spare for fading later |
| D6 | PB1 | buzzer, NPN base via 1 kR | also PWM-capable, unused |
| D2 | PB3 | future button A | reserved |
| D4 | PA12 | future button B | reserved, also FDCAN1_TX |
| D3 | PB0 | spare | |
| D0/D1 | PB7/PB6 | untouched | USART1 |
| A0-A5 | | unused | not 5 V tolerant |

Onboard, no wiring needed: the 8x13 blue LED matrix, and RGB LEDs LED3/LED4
(active-low, driven by the MCU). LED1/LED2 belong to Linux.

JMEDIA, JMISC and JCTL are 1.8 V banks with a 2.1 V absolute maximum. They must
never see a 3.3 V signal.

Only D3, D5, D6, D9, D10 and D11 can do PWM, and D10/D11 are also the SPI chip
select and MOSI. Since the buzzer is active and needs no PWM, none of this
constrains the design.

---

## 3. Wiring

### RC522

| RC522 | UNO Q |
|---|---|
| VCC | **3V3** (never 5 V, the chip is a 3.3 V part) |
| GND | GND |
| RST | D5 |
| SDA / SS | D10 |
| SCK | D13 |
| MOSI | D11 |
| MISO | D12 |
| IRQ, EA | not connected |

IRQ is left unconnected on purpose. The design polls the reader from a
non-blocking state machine, so an interrupt line would add a pin and a race
without removing any work.

Module silkscreen varies between sellers. Match the label, not the pin position.

### Lives LEDs

`pin -> 150 R -> LED anode (long leg) -> cathode -> GND`. The pin sources current,
so these are active-high. That is the opposite polarity to the onboard RGB LEDs,
and mixing the two up is the most likely wiring bug in this project.

For a red LED at Vf 2.0 V:

```
R = (3.3 - 2.0) / 0.009 = 144 R  ->  150 R (E12)  ->  I = 8.7 mA
```

Comfortably under the ~20 mA per-pin limit, and 3 x 8.7 = 26 mA total leaves
headroom on the port budget. Other colours need a different resistor because Vf
changes: green at 150 R gives ~8 mA, but blue or white at 150 R gives only ~2 mA
and is nearly invisible, so use 68 R for those.

### Buzzer

TMB12A05, an active electromagnetic buzzer with a built-in oscillator. Low-side
NPN switch:

| # | From | To |
|---|---|---|
| 1 | D6 | 1 kR -> transistor base |
| 2 | transistor base | 10 kR -> GND |
| 3 | transistor emitter | GND |
| 4 | transistor collector | buzzer `-` |
| 5 | buzzer `+` | 5V pin |
| 6 | 1N4007 cathode (stripe) | buzzer `+` |
| 7 | 1N4007 anode | buzzer `-` |

Base resistor maths, worst case across a 2N2222A and a BC547:

```
I_C = 50 mA (design margin over the 20.3 mA the part actually draws)
forced beta = 25, so I_B = 50 / 25 = 2 mA
R_B = (3.3 - 0.7) / 0.002 = 1300 R  ->  1 kR, giving I_B = 2.6 mA
```

2.6 mA out of D6 is well inside the pin budget. Transistor dissipation is
0.3 V x 50 mA = 15 mW against a TO-92 rating around 500 mW, so no heatsink.

The 10 kR base-emitter pulldown holds the NPN off while D6 floats during reset,
so the buzzer cannot scream at power-on. The 1N4007 absorbs back-EMF from the
coil. Both are cheap insurance.

Transistor pinout differs by part and by manufacturer. With the flat face toward
you, left to right: PN2222A is E-B-C, but P2N2222A and BC547 are C-B-E. The base
is the middle pin in all three, emitter and collector are swapped. Read the
marking on the body, look up that marking, and confirm with a multimeter diode
test before powering on.

No series resistor on the buzzer. The part needs at least 4.0 V across it, and at
~20 mA a 47 R resistor would drop ~0.95 V which, together with the transistor's
saturation drop, can put it below its minimum operating voltage. Reduce loudness
with shorter beeps or by damping the sound hole instead.

### Power

The board requests a 5 V / 3 A USB-C contract and nothing higher. Use a supply and
cable rated for that. Bring-up can run from a laptop USB-C port, but the demo runs
from a 5 V / 3 A wall charger, because a laptop port (900 mA on USB-A 3.0, ~1.5 A
on USB-C without PD) can droop during Wi-Fi bursts.

There is one USB-C port, so it is either the laptop cable (power plus data plus
ADB) or the wall charger (power only, with App Lab over Network Mode). Not both.

---

## 4. Bridge protocol

Version 1. Design rules:

- **Every payload is a single JSON string** in both directions. The real handlers
  are templated on MsgPack types rather than fixed to `String fn(String)`, and
  relying on composite type mapping is a guess this project does not need to make.
- **Every MCU handler is registered with `provide_safe`**, which runs in the
  `loop()` context. `provide()` runs on a background RPC thread where Arduino APIs
  are unsafe.

  **Verified, not assumed.** The core's `main` (cores/arduino/main.cpp,
  arduino:zephyr 1.1.0) is:

  ```c
  for (;;) { loop(); __loopHook(); }
  ```

  `__loopHook` is a weak symbol that Arduino_RouterBridge overrides to call
  `safeUpdate()`, which dispatches every `provide_safe` handler. So safe handlers
  run on the **same thread as `loop()`, strictly between iterations, never during
  one**. Two consequences the firmware relies on:

  1. No synchronisation is needed anywhere. Producers and consumers cannot
     interleave, so a mutex would add only a way to deadlock. There is no
     `volatile` and no atomic in the production sketch.
  2. A slow handler stalls `loop()`, and with it the RFID poll, the countdown and
     every animation. Handlers therefore parse one small object, mutate state and
     return; all consequences are worked out in the loop.

- **Payloads are single JSON strings**, which binds natively because MsgPack's
  `str_t` *is* `arduino::String` on this core (`MsgPack/Types.h`:
  `using str_t = String;` under `#ifdef ARDUINO`). A `String fn(String)` handler
  needs no composite type mapping. Python must send `json.dumps(...)`, not a dict.
- MCU handlers never call `Bridge.call()` or print. The manual warns explicitly
  that starting a new communication while responding to one deadlocks.
- The MCU never blocks on Python. Events go into a static ring buffer and Python
  polls.
- No heap `String` churn in hot paths. JSON is built into fixed `char` buffers.

### Python -> MCU

| Method | Request | Response |
|---|---|---|
| `pt.get_status` | `{}` | `{proto, fw, state, uptime_ms, rfid_ok, matrix_ok, lives, round, score, queue_len, overflows, link_ok, rfid_version, rfid_version_documented, rfid_state}` |
| `pt.get_events` | `{since: seq}` | `{events: [{seq, t_ms, type, data}], oldest_seq, overflow}` |
| `pt.ack_events` | `{seq}` | `{ok}` |
| `pt.set_state` | `{state}` | `{ok}` |
| `pt.start_round` | `{round, limit_ms}` | `{ok}` |
| `pt.end_round` | `{result}` | `{ok}` |
| `pt.set_lives` | `{n}` | `{ok}` |
| `pt.show_score` | `{score}` | `{ok}` |
| `pt.buzz` | `{pattern}` | `{ok}` |
| `pt.selftest` | `{}` | `{leds, buzzer, matrix, rgb, rfid, details}` |

States: `boot`, `selftest`, `idle`, `tapped`, `playing`, `over`, `busy`, `error`.
Buzz patterns: `welcome`, `join`, `ok`, `wrong`, `tick`, `busy`, `over`, `error`.

`boot` and `selftest` were added during Stage 1b. Collapsing startup into `idle`
would make a boot that hangs inside the selftest indistinguishable from a healthy
idle console, which is precisely the failure worth seeing. **Python must treat an
unrecognised state as "not ready" rather than crashing.** Python may set `idle`,
`tapped`, `playing`, `over`, `busy` and `error`; `boot` and `selftest` describe the
MCU's own startup and are not settable.

### Two fields that do not mean what they look like they mean

**`matrix_ok` is not a test.** `Arduino_LED_Matrix::begin()` is
`int begin() { matrixBegin(); return 1; }` — it returns 1 unconditionally, and an
LED matrix has no readback of any kind, so software cannot tell whether any light
came out. The field records that initialisation ran. `pt.selftest` also reports
`matrix_has_readback: false`. Confirm the panel by eye.

**`rfid_ok` tolerates counterfeit dies.** Only version `0x00` and `0xFF` mean
"nothing is answering". Every other value, however undocumented, is a working
reader. The module on this bench reports `0x82`, a counterfeit die, and reads cards
perfectly. Treating an unknown version as a fault produced four false failures
during bring-up; see `HARDWARE.md`.

### MCU -> Python (queued, delivered via `pt.get_events`)

| Event | Data | Trigger |
|---|---|---|
| `card_tap` | `{uid_hex, uid_len, t_ms}` | new UID read and cooldown passed |
| `card_removed` | `{}` | card left the field |
| `round_timeout` | `{round}` | countdown hit zero before `end_round` |
| `button` | `{id, t_us}` | future, `micros()` timestamped |
| `hw_fault` | `{code}` | hardware failure |
| `heartbeat` | `{uptime_ms}` | periodic liveness |

The ring holds 16 slots, statically allocated, with a monotonic `uint32` sequence
number. Python polls roughly every 100 ms and acks by cursor. An `overflow` flag
tells Python it missed events so it can resync from `oldest_seq` and log it.

`uid_hex` is privacy-sensitive. Python HMACs it immediately and a logging filter
redacts the key, so the raw UID never reaches disk or a log line.

### Link-loss safety

If the MCU receives no Python heartbeat for 15 s it returns to `idle` and shows a
link-lost animation. If Python sees no MCU heartbeat for 5 s the kiosk shows a
hardware-fault banner and the game pauses.

### Error codes

`E_RFID_INIT`, `E_RFID_READ`, `E_MATRIX`, `E_BRIDGE`, `E_BUSY`, `E_COOLDOWN`,
`E_TOKEN`, `E_EXPIRED`, `E_PIN`, `E_RATE`, `E_NICKNAME`, `E_CONSENT`, `E_DB`,
`E_CORPUS`, `E_INTERNAL`.

---

## 5. State machines

### MCU

```
BOOT -> SELFTEST -> IDLE
IDLE -> TAPPED      (card_tap queued, then awaits Python)
IDLE -> BUSY        (pt.set_state(busy))
TAPPED -> PLAYING   (set_state(playing) + start_round)
TAPPED -> IDLE      (session rejected or expired)
PLAYING -> PLAYING  (end_round + start_round for the next one)
PLAYING -> OVER
OVER -> IDLE
BUSY -> IDLE
ERROR -> IDLE       (recovery attempt)
```

Autonomous non-blocking sub-machines, all `millis()` based, never `delay()` in
`loop()`:

1. **RFID poll.** `SEEK -> HALT_SELECT -> QUEUE_EVENT -> PRESENT_WATCH ->
   COOLDOWN -> SEEK`. Removal detection probes with WUPA, not REQA, because a
   halted card does not answer REQA.
2. **Countdown.** Active in PLAYING. Tick cadence derives from remaining time,
   `clamp(remaining/8, 60 ms, 500 ms)`, so ticking accelerates as time shrinks.
3. **Matrix animation.** State-driven frame sequences at an 80 ms tick, rendered
   from static frame tables with no per-frame allocation.
4. **Buzzer rhythm.** A pattern is a static array of on/off pairs.
5. **RGB status.** LED3 shows state colour, LED4 pulses while awaiting a phone.

### Server session

```
IDLE -> AWAITING_PHONE   (card_tap: create session, token + PIN, 90 s TTL)
IDLE -> BUSY_SIGNAL      (tap while busy: no new session)
AWAITING_PHONE -> CONSENT      (phone connects, player is new)
AWAITING_PHONE -> ROUND_START  (phone connects, consent already stored)
AWAITING_PHONE -> IDLE         (TTL expired)
CONSENT -> ROUND_START
ROUND_START -> AWAITING_ANSWER
AWAITING_ANSWER -> ROUND_RESULT   (answer received or deadline passed)
ROUND_RESULT -> ROUND_START       (lives > 0 and pool not exhausted)
ROUND_RESULT -> GAME_OVER
GAME_OVER -> REVIEW -> IDLE
```

The server is the single source of truth for timing. The phone renders a countdown
from a deadline it is given, the MCU runs its own countdown for ticking and
timeout events, and if the two disagree the server wins.

Reaction time is `t_server(answer) - t_server(render_ack)`, measured from the
phone's `message_rendered` event. If no ack arrives within 2 s of `round_start`,
fall back to `round_start` and flag `ack_missing` so the data stays honest.

---

## 6. Game rules

Survival mode. Round time is `max(4, 14 - 0.8 * round_index)` seconds, so 14.0 s
down to a 4.0 s floor at round 13. Three lives. Timeout counts as wrong. The game
ends at zero lives or when the item pool is exhausted.

### Scoring

For a correct answer:

```
frac  = clamp(remaining_ms / limit_ms, 0, 1)
speed = floor(8 * frac)
mult  = min(3.0, 1.0 + 0.25 * streak_before)
pts   = floor((10 + speed) * mult)
```

`streak_before` is the run of consecutive correct answers before this one, reset
to zero by any wrong answer or timeout. Wrong or timeout scores zero.

Range is 10 points for a slow answer with no streak up to 54 for an instant answer
at a streak of 8 or more.

### Item selection

Early rounds draw from `easy`, later rounds from `hard`, sampled without
replacement within a session and balanced across channels so the same format does
not repeat consecutively when the pool allows.

One active session at a time. Tapping while busy shows BUSY on the matrix and
plays the `busy` rhythm without creating a session. Session tokens are 128-bit
random and expire after 90 s if no phone connects, and again at game end. The same
card has a 3 s cooldown.

Correct answers are not sent to the phone until the game ends.

---

## 7. Web

Served by the `web_ui` brick on port 7000. It is FastAPI plus Uvicorn plus
Socket.IO, so the phone client must vendor the Socket.IO JS client.

Pages: `kiosk` (attract mode, QR and PIN, live leaderboard, mirror of the current
game, final score), `game` (phone, mobile-first), `review`, `join` (PIN entry
fallback), `admin` (PIN protected).

The kiosk is a laptop browser tab pointed at the board. The laptop runs no server
and needs no software installed.

### REST

Query strings only, no path parameters, since support for those is unverified.

| Endpoint | Notes |
|---|---|
| `GET /api/health` | `{ok, version, base_url, uptime_s, bridge_ok}` |
| `GET /api/kiosk` | state, session URL, PIN, round, lives, score, leaderboard |
| `GET /api/session?token=` | session and current round |
| `POST /api/session/connect` | token, rate limited to 10/min/IP |
| `POST /api/join` | PIN fallback, 10/min/IP, lockout after 5 wrong |
| `POST /api/consent` | token, nickname, consent version |
| `POST /api/render_ack` | starts the reaction-time clock |
| `POST /api/answer` | 3 per round |
| `GET /api/review?token=` | valid 10 min after game over |
| `GET /api/leaderboard` | top 10 |
| `POST /api/admin/login` | 5/min/IP |
| `GET /api/admin/export` | anonymised CSV |
| `POST /api/admin/player/delete` | |
| `POST /api/admin/db/reset` | requires `confirm=RESET` |
| `POST /api/admin/clock` | sets the wall-clock offset |
| `POST /api/delete_request` | player-facing, token scoped |

Rate limiting is an in-memory token bucket per IP and per token, on a monotonic
clock. It survives nothing, which is fine: a restart clearing the buckets is the
correct failure mode.

### WebSocket

Rooms are joined with the brick's built-in `enter_room` event. The phone joins a
room named for its token, the kiosk joins `"kiosk"`.

Client to server: `message_rendered`, `answer_ws`.
Server to client: `kiosk_state`, `session_started`, `round_start`,
`round_result`, `game_over`, `hw_fault`, `error`.

The countdown is rendered client-side from a deadline, so there is no per-second
traffic. REST `/api/answer` remains the authoritative fallback if the socket
drops.

`navigator.vibrate` is used only behind feature detection, because iOS Safari does
not support it, and there is always a visual flash fallback.

### Security and rendering

All message text is escaped. Links are defanged server-side before transmission
(`https://` becomes `hxxps://`, dots in hosts become `[.]`) and rendered as plain
text, never as anchors. Tokens are random. No external requests: the QR library and
the Socket.IO client are both vendored into `assets/`.

The QR is generated client-side with nayuki/QR-Code-generator, MIT licensed, about
15 KB, no dependencies. It was chosen over `davidshimjs/qrcodejs` because that one
has been unmaintained since 2015.

The kiosk also shows the URL as text, and the footer always shows the current
`base_url`, because DHCP can change the board's address.

The 8x13 matrix can never display a QR code. The smallest QR is 21x21 modules.

---

## 8. Data and privacy

SQLite via the Python standard library, in WAL mode so the Socket.IO callback
threads can read concurrently. The database lives in the app's `data/` folder,
which persists across restarts but is excluded from app exports unless "include
data" is ticked.

Tables: `meta`, `players`, `sessions`, `answers`, `items`, `tap_log`.

Every row stores wall-clock time and a monotonic timestamp. The board may have no
RTC and may be offline, so all durations and analytics use monotonic deltas, which
are immune to clock jumps. `meta.clock_offset_ms` is admin-settable and
`sessions.clock_offset_ms` snapshots the offset in force when the session started.

### Identity

```
player_id = HMAC-SHA256(server_secret, identity_bytes).hexdigest()[:16]
```

`server_secret` is 32 random bytes generated once at first boot, stored mode 0600,
never in the database, never logged. The raw UID exists only in the bridge event
payload, the HMAC input, and then nowhere. A logging filter redacts any `uid_hex`
key. This is pseudonymisation, not anonymisation, and the consent screen says so.

An `IDENTITY_MODE` flag selects the identity bytes:

- `shared_card` (demo default): `uid || 0x00 || normalised_nickname`. The demo uses
  one card for several people, so without the nickname every player collapses into
  a single row.
- `per_card` (production): `uid` only, with the nickname as a display label. Each
  student owns a card, so the UID alone is enough and stays distinct even if two
  people pick the same nickname.

Same card plus same nickname gives the same player, so consent is not re-shown and
history is preserved.

Nicknames are 3 to 12 characters, NFKC normalised, trimmed, case-folded for
identity purposes, and run through a simple profanity filter.

Delete-my-data lives on the phone review page, where the token identifies the exact
player, and in admin. It is deliberately not behind a card tap, because a shared
card cannot disambiguate who is asking. Deletion hard-deletes the player's
`answers`, `sessions` and `tap_log` rows and tombstones the `players` row with a
randomised nickname so the leaderboard stays honest.

### Analytics

Per-player and cohort accuracy broken down by cue type: urgency, sender mismatch,
lookalike domain, generic greeting, suspicious link, credential or OTP request,
unexpected attachment, too-good-to-be-true. Reaction-time distribution split by
`answer_source`, never mixing phone and button timings. A funnel of taps to
sessions started to sessions completed.

---

## 9. Content

`corpus.json` holds 40 items with the schema `id, channel, label, difficulty,
sender, subject, body, cues[], explanation, lesson`.

Channels are email, sms, whatsapp and poster. Labels are 50/50 phish to safe, and
difficulty is 20 easy to 20 hard, reasoned with the NIST Phish Scale idea of cue
count and context fit.

Content is India-relevant: bank KYC, UPI refund, scholarship portal, placement
cell, college portal, courier, electricity bill, WhatsApp forward.

Phishing items use only non-resolving or reserved domains (`.example`, `.invalid`)
that visually imitate real ones. Safe items use neutral content. No live malicious
URLs and no real personal data.

The corpus lives server-side at `python/corpus.json`, never under `assets/`. If it
were web-served the answers would be publicly downloadable. A test asserts this.

---

## 10. File tree

```
phishtap-lite/
  app.yaml                  App Lab manifest, declares the web_ui brick
  README.md
  docs/
    ARCHITECTURE.md         this file
    HARDWARE.md             verified BOM, power and network notes
  sketch/
    hw_selftest/            bring-up diagnostic, Stage 1a
    phishtap/               production firmware, Stage 1b
  python/
    main.py                 thin adapter only
    requirements.txt
    corpus.json             server-side, never web-exposed
    phishtap/
      config.py  clock.py  scoring.py  engine.py  sessions.py
      corpus.py  db.py  analytics.py  privacy.py  ratelimit.py  api.py
      adapters/
        bridge_adapter.py   the only file importing arduino.app_utils
        mock_bridge.py      laptop dev
        web_adapter.py      the only file importing app_bricks
  assets/
    index.html  game.html  review.html  join.html  admin.html
    css/app.css
    js/strings.js           every UI string, one file, translation ready
    js/common.js  kiosk.js  game.js  review.js  join.js  admin.js
    libs/                   vendored QR generator, Socket.IO client, licences
  data/                     runtime: phishtap.db, server_secret.key, admin_pin
  tests/                    laptop pytest, mock bridge, in-memory sqlite
  tools/                    verification and deploy scripts
```

---

## 11. Risks

| # | Risk | Mitigation |
|---|---|---|
| R1 | MFRC522 on the Zephyr core is unproven | **RESOLVED.** Phase B read UID `C7 DA 41 04`, SAK 0x08, 5/5 identical, removal detected. The module is a counterfeit die (version `0x82`) and works fine |
| R2 | matrix and RC522 might share a bus | **RESOLVED.** Phase A and Phase B both pass in the same run, so there is no conflict |
| R3 | 3V3 rail budget under RF burst plus LEDs | measure during the selftest, stagger LED and RF activity |
| R4 | Socket.IO client and server version mismatch | pin the vendored client after checking the board |
| R5 | bridge deadlock from a nested call | **RESOLVED structurally.** `provide_safe` handlers run on the loop thread between iterations, never concurrently, and none call back into the bridge or print |
| R6 | corpus answer leak | server-side only, plus a test asserting nothing under `assets/` |
| R7 | `data/` excluded from app exports | admin CSV export as backup, documented `scp` recovery |
| R8 | no RTC | monotonic deltas everywhere, admin-settable offset |
| R9 | a blocking handler stalls Socket.IO | short transactions, WAL mode, no network calls in handlers |
| R10 | `String` heap fragmentation on the MCU | fixed `char` buffers, static ring of POD structs |
| R11 | phone disconnects mid-round | server deadline grades the timeout regardless, token reconnect resumes |
| R12 | transistor emitter and collector swapped | documented per-part pinouts, audible selftest before any game logic |
| R13 | shared demo card collapses identity | `IDENTITY_MODE` flag, nickname folded into the HMAC |
| R14 | college Wi-Fi client isolation blocks phone to board | laptop hotspot as the default, mandatory pre-flight test |

---

## 12. Stages

- **Stage 0** — this document.
- **Stage 1a** — `hw_selftest` sketch covering bring-up phases A, B, C1, C2, C3,
  plus the no-write policy checker. Done.
- **Stage 1b** — production sketch: RC522 state machine, matrix/LED/buzzer
  engines, bridge handlers, event queue, heartbeat, link-loss safety. **Written
  and compiling clean with the real Zephyr toolchain on the board** (125 KB flash,
  54 KB RAM). `pt_json.h` and `pt_events.h` are Arduino-free and carry 327 host
  assertions under ASan/UBSan, plus independent validation of the emitted JSON by
  Python's parser. Not yet run on hardware. See `sketch/phishtap/README.md`.
- **Stage 2** — Python backend: engine, scoring, sessions, SQLite, analytics,
  identity modes, bridge adapter, web_ui adapter, mock bridge, laptop dev server,
  pytest.
- **Stage 3** — frontend: kiosk, game, review, join, admin, bundled QR library,
  styling, i18n strings.
- **Stage 4** — content: `corpus.json` with 40 items, validator, authoring guide.
- **Stage 5** — integration and docs: EndeavourOS guide, hotspot and pre-flight
  scripts, App Lab import, demo script, troubleshooting table, licences.
- **Stage 6** — optional physical PHISH/SAFE buttons on D2 and D4 with
  MCU-timestamped reaction times.
- **Stage 7** — optional analytics report generator and thesis appendix.
