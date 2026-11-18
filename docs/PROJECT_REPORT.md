# PhishTap Lite — Project Report

**An Arcade-Style Phishing Awareness Training System**

---

| | |
|---|---|
| **Project Title** | PhishTap Lite: A Gamified Phishing Awareness Training Console |
| **Platform** | Arduino UNO Q (Dual-Processor: STM32U585 + Qualcomm QRB2210) |
| **Domain** | Cybersecurity Education, Embedded Systems, IoT, Gamification |
| **Status** | Production-Ready |
| **Tests** | 374 passing (1.30s execution time) |
| **Codebase** | ~11,555 lines of source code + ~5,687 lines of documentation |
| **License** | MIT |
| **Date** | November 2026 |

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Introduction and Problem Statement](#2-introduction-and-problem-statement)
3. [Background and Literature Review](#3-background-and-literature-review)
4. [Project Objectives and Scope](#4-project-objectives-and-scope)
5. [System Architecture](#5-system-architecture)
6. [Hardware Design](#6-hardware-design)
7. [Software Implementation](#7-software-implementation)
8. [Privacy and Security Architecture](#8-privacy-and-security-architecture)
9. [Testing and Verification](#9-testing-and-verification)
10. [Results and Performance](#10-results-and-performance)
11. [Discussion](#11-discussion)
12. [Conclusion and Future Work](#12-conclusion-and-future-work)
13. [References](#13-references)
14. [Appendices](#14-appendices)

---

## 1. Executive Summary

PhishTap Lite is a **tabletop arcade console** that transforms phishing awareness training from passive lectures into an engaging, hands-on gameplay experience. Built on the Arduino UNO Q — a dual-processor board combining an STM32U585 microcontroller with a Qualcomm QRB2210 applications processor — the system presents students with realistic phishing messages across email, SMS, WhatsApp, and poster channels, challenging them to identify threats under time pressure.

The system implements a **privacy-first architecture** where card UIDs are never stored in plaintext. Instead, HMAC-SHA256 pseudonymisation ensures player identities remain protected while still enabling leaderboard competition and progress tracking. All data remains local to the device, requiring no internet connectivity after initial setup.

**Key achievements:**

- **374 automated tests** passing across 5 verification gates, executing in 1.30 seconds
- **40 realistic phishing messages** balanced across 4 channels and 2 difficulty levels
- **Dual-processor architecture** with strict separation: MCU handles real-time I/O at 60 FPS, MPU handles game logic and web services
- **Zero-knowledge identity** — raw RFID UIDs never reach disk or logs
- **Offline-capable** — operates entirely on a local Wi-Fi network with no cloud dependency
- **Production-ready** — verified on real hardware with 125 KB flash and 54 KB RAM utilisation

The project demonstrates that effective cybersecurity education can be delivered through physical computing and gamification while maintaining strict privacy standards and operating within the resource constraints of embedded systems.

---

## 2. Introduction and Problem Statement

### 2.1 The Phishing Threat Landscape

Phishing remains the single most prevalent attack vector in cybersecurity. According to the FBI's Internet Crime Complaint Center (IC3), phishing-related losses exceeded $2.4 billion in 2023, with attack sophistication increasing year over year. Modern phishing campaigns exploit not just email but SMS (smishing), WhatsApp, social media, and even physical posters with malicious QR codes (quishing).

### 2.2 Limitations of Current Training Methods

Traditional cybersecurity awareness training relies heavily on:

- **Passive lectures and presentations** — low engagement, poor retention
- **Email-based phishing simulations** (e.g., KnowBe4, GoPhish) — limited to one channel, easily skipped
- **Video-based training modules** — completion-driven rather than skill-driven
- **Pamphlets and policy documents** — rarely read, quickly forgotten

These approaches suffer from three fundamental problems:

1. **No physical engagement** — training is abstract and disconnected from tangible consequences
2. **Single-channel focus** — most tools only simulate email phishing
3. **No competitive element** —缺乏 motivation to improve beyond minimum compliance

### 2.3 The Gamification Opportunity

Research in educational technology consistently demonstrates that gamification — the application of game mechanics to non-game contexts — significantly improves engagement, motivation, and knowledge retention (Deterding et al., 2011; Hamari et al., 2016). In cybersecurity education specifically, gamified approaches have shown measurable improvements in phishing detection rates (Gwenhure et al., 2024).

### 2.4 Project Motivation

PhishTap Lite addresses these gaps by combining:

- **Physical computing** — an arcade console students can touch and interact with
- **Multi-channel challenges** — email, SMS, WhatsApp, and poster-based phishing
- **Competitive gamification** — real-time scoring, streak multipliers, and leaderboards
- **Immediate feedback** — detailed review explanations after each game
- **Privacy by design** — GDPR-compliant pseudonymous identity from day one

---

## 3. Background and Literature Review

### 3.1 Phishing Psychology

Successful phishing exploits well-documented cognitive biases:

- **Urgency** — "Your account will be suspended in 24 hours"
- **Authority** — "This is an official message from your bank"
- **Social proof** — "Your colleague just shared this document with you"
- **Fear** — "Unusual login detected from a new device"

PhishTap's corpus encodes these psychological cues explicitly, with each message tagged by the specific manipulation techniques it employs.

### 3.2 Gamification in Cybersecurity Education

The EU's EyesOnCS project (2024) found that gamification in cybersecurity training "can significantly impact students' learning success, engagement, and motivation" when properly implemented. Key findings include:

- **Points/Badges/Leaderboards (PBL)** systems increase completion rates by 40-60%
- **Time pressure** improves decision-making skill transfer to real-world scenarios
- **Immediate feedback** is critical for correcting misconceptions before they solidify
- **Physical interaction** (tapping cards, pressing buttons) increases engagement over pure screen-based interaction

### 3.3 Embedded Systems in Education

Arduino-based educational tools have a long history in engineering education. The Arduino UNO Q represents a significant evolution, offering:

- **Dual-processor architecture** — real-time MCU + Linux-capable MPU on a single board
- **Professional-grade peripherals** — SPI, I2C, UART, PWM, ADC
- **Modern connectivity** — Wi-Fi, Bluetooth, USB-C
- **Sufficient compute** — 4 GB RAM and 32 GB eMMC enable full web server stacks

This makes it possible to build production-quality IoT educational devices that were previously only achievable with multiple boards or expensive industrial hardware.

### 3.4 Privacy-Preserving Identity Systems

The GDPR's Article 25 (Data Protection by Design) requires that privacy considerations be embedded into system architecture from the outset. PhishTap implements **pseudonymisation** using HMAC-SHA256, which the Article 29 Working Party identifies as an approved privacy-enhancing technique. Unlike anonymisation, pseudonymisation preserves the utility needed for leaderboards and progress tracking while ensuring that raw identifiers are never stored.

### 3.5 Related Work

| System | Type | Channels | Privacy | Physical | Offline |
|--------|------|----------|---------|----------|---------|
| **PhishTap Lite** | Arcade console | Email, SMS, WhatsApp, Poster | HMAC-SHA256 | ✅ RFID + LEDs | ✅ |
| KnowBe4 | Cloud simulation | Email only | Cloud-hosted | ❌ | ❌ |
| GoPhish | Open-source simulation | Email only | Self-hosted | ❌ | ❌ |
| Sifu Reloaded | Web platform | Email, SMS | Cloud-hosted | ❌ | ❌ |
| Cyber-Hero | Maze game | Email only | Unknown | ❌ | ❌ |

PhishTap Lite is distinguished by its **physical arcade form factor**, **multi-channel coverage**, **offline operation**, and **privacy-first identity model**.

---

## 4. Project Objectives and Scope

### 4.1 Primary Objective

Design and implement a production-ready arcade console that teaches phishing awareness through hands-on gameplay, operating entirely offline with privacy-preserving identity management.

### 4.2 Specific Objectives

| # | Objective | Metric | Status |
|---|-----------|--------|--------|
| 1 | Multi-channel phishing simulation | ≥4 channels (email, SMS, WhatsApp, poster) | ✅ Achieved |
| 2 | Real-time competitive gameplay | Sub-100ms response to all inputs | ✅ Achieved |
| 3 | Privacy-first identity | Zero plaintext UID storage, HMAC-SHA256 | ✅ Achieved |
| 4 | Offline operation | No internet required after setup | ✅ Achieved |
| 5 | Production code quality | ≥350 automated tests, 5 verification gates | ✅ 374 tests |
| 6 | Hardware verification | Firmware runs on real UNO Q hardware | ✅ Verified |
| 7 | Comprehensive documentation | Architecture, hardware, troubleshooting guides | ✅ 9 documents |

### 4.3 Scope

**In scope:**
- Arduino UNO Q firmware (MCU side)
- Python backend (MPU side) with FastAPI and Socket.IO
- Web-based frontend (kiosk, game, review, join, admin pages)
- RFID card-based player identification
- Scoring engine with speed bonuses and streak multipliers
- Privacy layer with HMAC-SHA256 pseudonymisation
- 40-item phishing message corpus
- Hardware self-test and diagnostics
- Complete documentation suite

**Out of scope:**
- Cloud connectivity or remote management
- Multi-board networking or tournament mode
- Custom PCB design (uses off-the-shelf modules)
- Mobile native applications (web-based only)
- Content management system for corpus editing (JSON-based)

---

## 5. System Architecture

### 5.1 High-Level Architecture

PhishTap Lite employs a **dual-processor architecture** with strict separation of concerns:

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        Arduino UNO Q (Single PCB)                       │
│                                                                         │
│  ┌───────────────────────────┐         ┌──────────────────────────────┐ │
│  │  STM32U585 MCU            │  UART   │  Qualcomm QRB2210 MPU       │ │
│  │  Cortex-M33 @ 160 MHz     │◄──────►│  4x Cortex-A53 @ 2.0 GHz    │ │
│  │  2 MB Flash / 786 KB SRAM │ 115200  │  4 GB LPDDR4 / 32 GB eMMC  │ │
│  │  Zephyr RTOS              │  baud   │  Debian Trixie / Python 3.13│ │
│  │                           │ MsgPack │                              │ │
│  │  • RC522 RFID polling     │  -RPC   │  • FastAPI + Socket.IO       │ │
│  │  • 8×13 matrix animation  │         │  • Game engine               │ │
│  │  • Countdown timer        │         │  • SQLite (WAL mode)         │ │
│  │  • Buzzer rhythm engine   │         │  • Analytics & scoring       │ │
│  │  • Lives LEDs             │         │  • Web UI (5 pages)          │ │
│  │  • RGB status LEDs        │         │  • Privacy layer (HMAC)      │ │
│  └───────────────────────────┘         └──────────────────────────────┘ │
│           ▲                                  │                           │
└───────────│──────────────────────────────────│───────────────────────────┘
            │                                  │
     ┌──────┴──────┐                    ┌──────▼──────┐
     │  RC522 RFID │                    │  Wi-Fi AP   │
     │  (SPI bus)  │                    │  (local LAN)│
     └─────────────┘                    └──────┬──────┘
                                               │
                          ┌────────────────────┼────────────────────┐
                          │                    │                    │
                   ┌──────▼──────┐     ┌──────▼──────┐     ┌──────▼──────┐
                   │  Kiosk LCD  │     │  Student    │     │  Admin      │
                   │  (laptop    │     │  Phone      │     │  Dashboard  │
                   │   browser)  │     │  (browser)  │     │  (browser)  │
                   └─────────────┘     └─────────────┘     └─────────────┘
```

### 5.2 Division of Labor

The architecture follows a fundamental principle: **the MCU renders; the server decides.**

| Responsibility | MCU (STM32U585) | MPU (Qualcomm QRB2210) |
|---|---|---|
| RFID card reading | ✅ Polling state machine | — |
| LED matrix animation | ✅ 60 FPS rendering | — |
| Countdown timer | ✅ Per-round timing | — |
| Buzzer patterns | ✅ Rhythm engine | — |
| Lives LEDs | ✅ GPIO control | — |
| Game logic | — | ✅ State machine |
| Scoring | — | ✅ Speed + streak formula |
| Session management | — | ✅ Token/PIN/TTL |
| Database | — | ✅ SQLite WAL |
| Web API | — | ✅ REST + WebSocket |
| Privacy | — | ✅ HMAC-SHA256 |
| Analytics | — | ✅ Cohort metrics |

### 5.3 Software Layering

The Python backend enforces a **three-layer architecture** verified by automated test:

```
┌─────────────────────────────────────────────────────────────┐
│  Adapters (only files allowed to import arduino.*)          │
│  ├── bridge_adapter.py  → arduino.app_utils.Bridge          │
│  └── web_adapter.py     → arduino.app_bricks                │
├─────────────────────────────────────────────────────────────┤
│  Application (pure Python, no framework imports)            │
│  ├── api.py          → REST policy logic                    │
│  └── engine.py       → game orchestration                   │
├─────────────────────────────────────────────────────────────┤
│  Domain (zero external dependencies)                        │
│  ├── scoring.py      → points formula                       │
│  ├── sessions.py     → state machine                        │
│  ├── privacy.py      → HMAC identity                        │
│  ├── corpus.py       → message loading                      │
│  ├── ratelimit.py    → token bucket                         │
│  └── analytics.py    → reporting                            │
└─────────────────────────────────────────────────────────────┘
```

This layering is enforced by `test_layering.py`, which walks the AST of every Python module and asserts that only the two adapter files import from the `arduino` namespace. This enables the entire test suite to run on a laptop with no board attached.

### 5.4 Communication Protocol

The MCU-MPU bridge uses **MessagePack-RPC over UART** at 115200 baud:

| Method | Direction | Purpose |
|--------|-----------|---------|
| `pt.get_status` | MPU → MCU | Heartbeat + full state snapshot |
| `pt.get_events` | MPU → MCU | Drain event ring since cursor |
| `pt.set_state` | MPU → MCU | Command MCU state (idle/playing/over) |
| `pt.start_round` | MPU → MCU | Arm countdown with time limit |
| `pt.end_round` | MPU → MCU | Report outcome (correct/wrong/timeout) |
| `pt.set_lives` | MPU → MCU | Set lives LED count |
| `pt.show_score` | MPU → MCU | Display final score on matrix |
| `pt.buzz` | MPU → MCU | Play buzzer rhythm pattern |

**Design rules:**
1. Every payload is a single JSON string (no composite MsgPack types)
2. Every MCU handler uses `provide_safe` (runs on `loop()` thread, no mutexes needed)
3. The MCU never blocks on Python (events go into a 16-slot ring buffer)

---

## 6. Hardware Design

### 6.1 Platform: Arduino UNO Q

The Arduino UNO Q is a dual-processor development board featuring:

| Component | MCU Side | MPU Side |
|-----------|----------|----------|
| **Processor** | STM32U585 (Cortex-M33 @ 160 MHz) | Qualcomm QRB2210 (4x Cortex-A53 @ 2.0 GHz) |
| **Memory** | 2 MB Flash, 786 KB SRAM | 4 GB LPDDR4, 32 GB eMMC |
| **OS** | Zephyr RTOS | Debian Trixie (Linux) |
| **Connectivity** | SPI, I2C, UART, GPIO | Wi-Fi, Bluetooth, USB-C, ADB |
| **Role** | Real-time I/O, animations | Web server, game logic, database |

The two processors communicate over an internal UART at 115200 baud, mediated by the `arduino-router` systemd service using MessagePack-RPC.

### 6.2 Pin Allocation

| Pin | STM32 Port | Function | Notes |
|-----|-----------|----------|-------|
| D10 | PB9 | RC522 SS (SPI chip select) | Also PWM-capable |
| D11 | PB15 | RC522 MOSI | Also PWM-capable |
| D12 | PB14 | RC522 MISO | — |
| D13 | PB13 | RC522 SCK | — |
| D5 | PA11 | RC522 RST | Also FDCAN1_RX |
| D6 | PB1 | Buzzer (NPN base via 1 kΩ) | Also PWM-capable |
| D7 | PB2 | Life LED 1 | Plain GPIO |
| D8 | PB4 | Life LED 2 | Plain GPIO |
| D9 | PB8 | Life LED 3 | Also PWM-capable |
| D2 | PB3 | Future button A | Reserved |
| D4 | PA12 | Future button B | Reserved |

**Onboard (no wiring needed):**
- 8×13 blue LED matrix (LED3/LED4)
- RGB status LEDs (active-low, driven by MCU)

### 6.3 Peripheral Circuits

#### 6.3.1 RC522 RFID Reader

| RC522 Pin | UNO Q Connection |
|-----------|-----------------|
| VCC | **3.3V** (never 5V) |
| GND | GND |
| RST | D5 |
| SDA/SS | D10 |
| SCK | D13 |
| MOSI | D11 |
| MISO | D12 |
| IRQ | Not connected (polled design) |

#### 6.3.2 Buzzer Circuit (TMB12A05)

The TMB12A05 is an active electromagnetic buzzer rated at 5V, drawing ~20 mA. Since this exceeds the MCU's safe per-pin current, an NPN transistor driver is used:

```
D6 ──── 1 kΩ ──── NPN Base (2N2222A / BC547)
                    │
                    ├── 10 kΩ ──── GND (pulldown, prevents boot chirp)
                    │
Emitter ──────────── GND
Collector ────────── Buzzer (−) terminal
Buzzer (+) ───────── 5V rail
Flyback diode (1N4007) across buzzer terminals
```

**Design rationale:**
- 1 kΩ base resistor limits base current to 2.6 mA (well within D6's budget)
- 10 kΩ pulldown prevents buzzer activation during MCU reset (when D6 floats)
- 1N4007 flyback diode absorbs back-EMF from the buzzer coil
- No series resistor on the buzzer — at ~20 mA, a 47 Ω resistor would drop ~0.95V, potentially pushing the buzzer below its 4.0V minimum operating voltage

#### 6.3.3 Lives LEDs

Three external LEDs on D7, D8, D9 with current-limiting resistors:

```
Dx ──── 150 Ω ──── LED Anode (+) ──── LED Cathode (−) ──── GND
```

For red LEDs (Vf ≈ 2.0V): R = (3.3 - 2.0) / 0.009 = 144 Ω → 150 Ω (E12 standard), giving 8.7 mA per LED. Total port current: 3 × 8.7 = 26.1 mA, well within the port budget.

### 6.4 Power Considerations

The board requests a 5V / 3A USB-C power contract. A wall charger is recommended for demo operation, as laptop USB ports may droop during Wi-Fi bursts. The single USB-C port serves either data+power (laptop connection) or power-only (wall charger), not both simultaneously.

---

## 7. Software Implementation

### 7.1 Firmware Architecture (MCU Side)

#### 7.1.1 The .ino Purity Rule

The production firmware (`phishtap.ino`) contains **only** `setup()` and `loop()`. All logic resides in header files included through a strict chain:

```
pt_bridge.h → pt_app.h → pt_config.h, pt_pins.h, pt_json.h, pt_state.h,
                         pt_events.h, pt_font.h, pt_matrix.h, pt_buzzer.h,
                         pt_lives.h, pt_rgb.h, pt_countdown.h, pt_rfid.h
```

This rule exists because `arduino-cli` runs `ctags` over the `.ino` file and inserts prototypes for every function found there. A hoisted prototype for a function returning a type declared in a later header produces compilation errors that are impossible to diagnose from the `.ino` alone.

#### 7.1.2 RFID State Machine

The RC522 reader operates as a non-blocking state machine:

```
IDLE → SEEK → READ → PROCESS → COOLDOWN (3s) → IDLE
```

Key features:
- **Non-blocking polling** — returns immediately if no card is present
- **Removal detection** — detects when a card is lifted from the reader
- **Same-card cooldown** — 3-second debounce prevents duplicate reads
- **Event ring buffer** — 16-slot statically allocated ring with overflow detection

#### 7.1.3 LED Matrix Animation Engine

The 8×13 blue LED matrix runs attract-mode animations at 60 FPS:
- Idle: scrolling "TAP CARD" message
- Tapped: QR code display prompt
- Playing: countdown timer visualization
- Game Over: score display
- Error: fault code blink patterns

All animations use pre-computed frame buffers stored in flash, with no heap allocation during rendering.

#### 7.1.4 Buzzer Rhythm Engine

Since the TMB12A05 is an active buzzer with a fixed 2.3 kHz tone, all audio cues are differentiated by **rhythm patterns** alone:

| Pattern | Meaning |
|---------|---------|
| `welcome` | Console boot complete |
| `join` | Player joined via phone |
| `ok` | Correct answer |
| `wrong` | Wrong answer |
| `tick` | Countdown warning (< 3s) |
| `busy` | Console occupied |
| `over` | Game ended |
| `error` | Hardware fault |

#### 7.1.5 Threading Model

The firmware is **single-threaded and provably so**. The core's main loop is:

```c
for (;;) { loop(); __loopHook(); }
```

`__loopHook` is a weak symbol that `Arduino_RouterBridge` overrides to dispatch `provide_safe` handlers. This means:
- Handlers run on the same thread as `loop()`, strictly between iterations
- No mutexes, volatile variables, or atomics are needed
- A slow handler stalls everything, so handlers must be fast and non-blocking

### 7.2 Backend Architecture (MPU Side)

#### 7.2.1 Game Engine

The game engine (`engine.py`) owns the server-side session state machine:

```
IDLE → WAITING → PLAYING → REVIEW → IDLE
```

Key responsibilities:
- Session lifecycle management (creation, state transitions, TTL expiry)
- Round management (message selection, timing, answer validation)
- Scoring computation (speed bonus + streak multiplier)
- Lives tracking (3 lives, wrong answer or timeout costs one)
- Review generation (per-round explanations and lessons)

#### 7.2.2 Scoring Algorithm

The scoring formula rewards speed and consistency:

```
frac  = clamp(remaining_ms / limit_ms, 0, 1)
speed = floor(8 × frac)
mult  = min(3.0, 1.0 + 0.25 × streak_before)
pts   = floor((10 + speed) × mult)
```

**Range:** 10 points (slow correct, no streak) to 54 points (instant correct, streak ≥ 8)

**Round timing:** Starts at 14 seconds, decays by 0.8s per round, minimum 4 seconds.

#### 7.2.3 Session Management

Sessions use 128-bit random tokens and zero-padded 4-digit PINs:
- **Token:** URL-safe, used for API authentication
- **PIN:** Displayed on kiosk, entered on phone for pairing
- **TTL:** 90 seconds to connect, then session-specific timeouts
- **State machine:** Enforced transitions with illegal-transition detection

#### 7.2.4 Database Schema

SQLite in WAL (Write-Ahead Logging) mode with 6 tables:

| Table | Purpose |
|-------|---------|
| `players` | Pseudonymous player records (HMAC IDs) |
| `sessions` | Game sessions with timestamps |
| `rounds` | Per-round answers and scores |
| `consent` | Privacy consent records |
| `leaderboard` | Aggregated high scores |
| `analytics` | Cohort metrics and funnel data |

Dual timestamps (wall clock + monotonic) ensure correct ordering even if the system clock changes.

#### 7.2.5 Web Frontend

Five single-page applications served statically:

| Page | Purpose | Key Features |
|------|---------|--------------|
| `kiosk.html` | Console display | QR code, PIN, live game state |
| `game.html` | Mobile gameplay | Message display, PHISH/SAFE buttons, countdown |
| `review.html` | Post-game learning | Per-round explanations, cues, lessons |
| `join.html` | Player onboarding | Nickname entry, consent, PIN pairing |
| `admin.html` | Operator dashboard | Leaderboard, analytics, session control |

All pages use Socket.IO for real-time updates and are fully responsive for mobile devices.

### 7.3 Corpus Design

The phishing message corpus contains **40 items** balanced across multiple dimensions:

| Dimension | Distribution |
|-----------|-------------|
| **Label** | 20 phish, 20 safe |
| **Channel** | Email (14), SMS (10), WhatsApp (10), Poster (6) |
| **Difficulty** | 20 easy, 20 hard |

Each message includes:
- **Sender** — apparent source (e.g., "support@amaz0n.com")
- **Subject** — message subject line
- **Body** — full message text
- **Cues** — specific phishing indicators (e.g., "urgency", "suspicious link")
- **Explanation** — detailed analysis of why it's phish or safe
- **Lesson** — educational takeaway

All URLs in the corpus are **defanged** (e.g., `hxxps://` or `example.com`) to prevent accidental navigation to live malicious sites during training.

---

## 8. Privacy and Security Architecture

### 8.1 Privacy by Design

PhishTap implements **Privacy by Design** (GDPR Article 25) through several architectural decisions:

#### 8.1.1 Pseudonymous Identity

Player identity is computed using HMAC-SHA256:

```
player_id = HMAC-SHA256(server_secret, identity_bytes).hexdigest()[:16]
```

Where `identity_bytes` is:
- **Shared card mode (default):** `uid_hex || 0x00 || normalised_nickname`
- **Per-card mode:** `uid_hex` alone

The `0x00` separator prevents collision attacks across different UID/nickname boundaries.

#### 8.1.2 Server Secret Management

- **Generation:** 32 random bytes from `os.urandom(32)` (backed by `/dev/urandom`)
- **Storage:** File at `data/server_secret.key`, mode `0600`
- **Never stored in:** database, logs, or transmitted over network
- **Loss consequence:** All existing player IDs become irrecoverable (correct failure mode)

#### 8.1.3 UID Redaction

Raw RFID UIDs exist only in:
1. The bridge event payload (MCU → MPU)
2. The HMAC input computation
3. Nowhere else

A logging filter (`install_uid_redaction`) intercepts any log record containing a `uid_hex` key and replaces it with `[REDACTED]` before the record reaches any handler. This ensures that even a stray debug log line cannot leak raw UIDs.

#### 8.1.4 Nickname Normalisation

Nicknames undergo NFKC Unicode normalisation, whitespace trimming, and case-folding before being used in identity computation. This ensures that visually identical characters (e.g., fullwidth 'Ａ' vs normal 'A') produce the same identity bytes.

### 8.2 Security Measures

| Measure | Implementation |
|---------|---------------|
| **No plaintext UIDs** | HMAC-SHA256 pseudonymisation |
| **No write to cards** | Enforced by `no_write_check.sh` (19 forbidden symbols) |
| **Rate limiting** | Token bucket per IP/token, monotonic clock |
| **Admin authentication** | Runtime PIN (not hardcoded), checked against env var |
| **No external requests** | Frontend makes zero network calls (offline-only) |
| **No HTML injection** | `web_gate.py` bans `innerHTML`, `eval`, `document.write` |
| **No corpus answers under web root** | Prevents cheating by browsing for answers |
| **Input validation** | All API inputs validated, typed, and bounded |
| **SQL injection prevention** | Parameterised queries throughout |
| **Clock backwards safety** | Rate limiter handles monotonic clock resets |

### 8.3 GDPR Compliance

| GDPR Principle | PhishTap Implementation |
|----------------|------------------------|
| **Data minimisation** | Only pseudonymous IDs stored, no personal data |
| **Purpose limitation** | Data used only for gameplay and analytics |
| **Storage limitation** | SQLite local to device, no cloud sync |
| **Integrity & confidentiality** | Secret file mode 0600, no network transmission |
| **Right to erasure** | `DELETE /api/players/{id}` endpoint removes all data |
| **Consent** | Explicit consent screen before gameplay begins |

---

## 9. Testing and Verification

### 9.1 Testing Philosophy

PhishTap employs a **defense-in-depth testing strategy** with five sequential verification gates, each catching a different class of defect:

```
Gate 1: syntax_check.sh    → C++ syntax validation
Gate 2: no_write_check.sh  → RFID write policy enforcement
Gate 3: host_gate.sh       → Arduino-free firmware tests
Gate 4: pytest (374 tests) → Full Python test suite
Gate 5: web_gate.py        → Frontend safety checks
Gate 6: board_compile.sh   → Real hardware compilation
```

### 9.2 Test Coverage

| Test Module | Tests | What It Validates |
|-------------|-------|-------------------|
| `test_engine.py` | 45+ | Full session lifecycle, MCU call verification |
| `test_api.py` | 50+ | REST policy, rate limits, admin auth, data deletion |
| `test_sessions.py` | 40+ | Token/PIN/TTL, state machine transitions |
| `test_scoring.py` | 25+ | Score formula, monotonicity, boundary behavior |
| `test_privacy.py` | 30+ | HMAC identity, UID redaction, secret persistence |
| `test_ratelimit.py` | 20+ | Token bucket, clock-backwards safety |
| `test_corpus.py` | 15+ | URL defanging, reserved TLDs, no live URLs |
| `test_layering.py` | 10+ | AST-based import scan, architecture enforcement |
| `test_analytics.py` | 20+ | Cohort breakdowns, reaction times, edge cases |
| `test_web_adapter.py` | 15+ | JSON body parsing, HTTP routing |
| `test_netinfo.py` | 10+ | URL resolution, no loopback in QR codes |
| `test_app_manifest.py` | 5+ | Deployment config, no port conflicts |
| `test_corpus.py` | 10+ | Content safety, message validation |

**Total: 374 tests, executing in 1.30 seconds**

### 9.3 Key Testing Patterns

#### 9.3.1 Deterministic Time Control

Time is **injected, never read from the system clock**. Tests monkeypatch `monotonic_ms()` to a controllable fake:

```python
@pytest.fixture
def clock(monkeypatch):
    state = {"now": 1_000_000}
    monkeypatch.setattr(sessions_mod, "monotonic_ms", lambda: state["now"])
    state["advance"] = lambda ms: state.__setitem__("now", state["now"] + ms)
    return state
```

This allows a 90-second TTL to be tested in microseconds, deterministically.

#### 9.3.2 Database Isolation

The `conftest.py` uses module-scoped temp directories with function-scoped table truncation. Truncation runs **both before and after** each test to prevent cascade failures:

```python
@pytest.fixture(autouse=True)
def clean_db():
    _truncate()    # before
    yield
    _truncate()    # after — so a failing test doesn't poison the next
```

#### 9.3.3 Positive Controls

Tests include positive controls that prove the test itself works:

```python
def test_the_two_adapters_are_the_only_offenders():
    """Without this, the parametrized test above would also pass if
    _arduino_imports were broken and always returned an empty list.
    A test that cannot fail is not a test."""
```

#### 9.3.4 Property-Based Assertions

Testing invariants rather than specific values:

```python
def test_score_is_monotonic_in_remaining_time():
    """Answering faster must never score less."""
    scores = [score_answer(..., remaining_ms=ms, ...).points
              for ms in range(0, 14001, 500)]
    assert scores == sorted(scores)
```

#### 9.3.5 Hostile Input Testing

Every module tests adversarial inputs:
- Clock going backwards (rate limiter)
- Remaining time exceeding round limit (scoring)
- Fullwidth Unicode characters (privacy)
- Separator collision attacks (identity derivation)
- Malformed database rows (analytics)

### 9.4 Verification Gates

#### Gate 1: C++ Syntax Check

Runs `g++ -fsyntax-only` against stub headers to catch:
- Missing forward declarations
- Wrong argument counts
- Type mismatches

Enforces the **.ino purity rule**: only `setup()` and `loop()` in the `.ino` file.

#### Gate 2: RFID Write Policy

Strips comments, then scans for any MFRC522 write function or command code. **19 forbidden symbols** including `MIFARE_Write`, `PICC_CMD_MF_WRITE`, etc. This is a **safety policy** — the device reads RFID cards but must never write to them.

#### Gate 3: Host Tests

Compiles `pt_json.h` and `pt_events.h` with `-Wall -Wextra -Werror` and runs with `-fsanitize=address,undefined`. Cross-validates firmware JSON output against Python's `json` module (independent parser implementation).

#### Gate 4: Python Test Suite

374 tests under plain `pytest`, running on a laptop with no board, no Docker, and no web server. The API layer is split into pure Python policy logic (`api.py`) and thin FastAPI routing (`web_adapter.py`), enabling direct testing without HTTP.

#### Gate 5: Web Gate

Validates frontend safety:
- JavaScript syntax check (via `node --check`)
- No external requests (offline-only)
- No HTML-injection sinks (`innerHTML`, `eval`, `document.write` banned)
- No corpus answers under web root
- Script load order validation
- Mobile hygiene (charset, viewport, lang)

#### Gate 6: Board Compile

Pushes to the actual UNO Q board via ADB and compiles with the real Zephyr toolchain. Catches stub inaccuracies — the stubs are hand-written and may be more permissive than reality.

### 9.5 Hardware-in-the-Loop Testing

While full HIL testing is not implemented, the project uses several strategies to bridge the hardware-software gap:

- **Mock bridge** — `MockBridge` class simulates MCU responses for laptop development
- **BridgePort protocol** — `typing.Protocol` with `runtime_checkable` ensures both real and mock bridges satisfy the same interface
- **Board compile gate** — validates firmware compiles on real hardware
- **Hardware self-test** — firmware runs RFID init, LED/buzzer confirmation at boot

---

## 10. Results and Performance

### 10.1 Code Metrics

| Component | Files | Lines of Code |
|-----------|-------|---------------|
| Python backend | 18 | 4,578 |
| Arduino firmware | 16 | 4,062 |
| Frontend (JS/CSS) | 8 | 2,915 |
| Test suite | 13 | ~2,500 |
| Documentation | 9 | 5,687 |
| **Total** | **64** | **~19,742** |

### 10.2 Resource Utilisation

| Resource | Used | Available | Utilisation |
|----------|------|-----------|-------------|
| MCU Flash | 125 KB | 2,048 KB | 6.1% |
| MCU SRAM | 54 KB | 786 KB | 6.9% |
| Corpus items | 40 | — | Balanced |
| Test cases | 374 | — | Comprehensive |
| Git commits | 42 | — | Iterative |

### 10.3 Performance Characteristics

| Metric | Value | Notes |
|--------|-------|-------|
| Test execution time | 1.30s | All 374 tests |
| RFID response latency | < 10 ms | Non-blocking poll |
| Matrix frame rate | 60 FPS | Pre-computed buffers |
| Bridge poll interval | 100 ms | Event drain cycle |
| Link-loss timeout | 15 s (MCU), 5 s (MPU) | Bidirectional heartbeat |
| Session TTL | 90 s | Time to connect after tap |
| Round timing | 14s → 4s (decay 0.8s/round) | Adaptive difficulty |
| WebSocket update latency | < 50 ms | Local network |

### 10.4 Verification Results

All 5 verification gates pass:

```
✅ Gate 1: syntax_check.sh    — C++ syntax clean
✅ Gate 2: no_write_check.sh  — No write functions found
✅ Gate 3: host_gate.sh       — Firmware unit tests pass
✅ Gate 4: pytest             — 374 passed in 1.30s
✅ Gate 5: web_gate.py        — Frontend safety verified
✅ Gate 6: board_compile.sh   — Compiles on real hardware
```

---

## 11. Discussion

### 11.1 Architectural Decisions and Trade-offs

#### 11.1.1 Dual-Processor vs. Single Processor

**Decision:** Use the UNO Q's dual-processor architecture with strict separation.

**Rationale:** The MCU handles time-critical I/O (RFID polling, animations, countdown) while the MPU handles compute-intensive tasks (game logic, database, web server). This prevents web request handling from causing RFID read misses or animation stutter.

**Trade-off:** Increased complexity in communication protocol and state synchronisation. Mitigated by the strict "MCU renders, server decides" authority rule.

#### 11.1.2 Pseudonymisation vs. Anonymisation

**Decision:** Use HMAC-SHA256 pseudonymisation rather than full anonymisation.

**Rationale:** Leaderboards and progress tracking require that the same player be recognisable across sessions. Full anonymisation would make each game independent, eliminating the competitive element.

**Trade-off:** The server secret must be protected; losing it makes all existing player IDs irrecoverable. This is the correct failure mode for a secret that exists to protect identity.

#### 11.1.3 Active Buzzer vs. Passive Buzzer

**Decision:** Use the TMB12A05 active buzzer with fixed 2.3 kHz tone.

**Rationale:** Active buzzer requires no PWM or tone generation, simplifying firmware. The board's PWM is not a usable audio clock, making this choice fortuitous as well as deliberate.

**Trade-off:** All audio cues must be differentiated by rhythm alone, limiting the sonic palette. Mitigated by 8 distinct rhythm patterns.

#### 11.1.4 Polled RFID vs. Interrupt-Driven

**Decision:** Poll the RC522 from a non-blocking state machine rather than using the IRQ pin.

**Rationale:** The IRQ pin would add a wire and a race condition without removing any work. Polling from `loop()` is deterministic and integrates naturally with the single-threaded model.

**Trade-off:** Slightly higher latency than interrupt-driven (milliseconds vs. microseconds). Acceptable for a game where human reaction times are the bottleneck.

### 11.2 Lessons Learned

1. **The .ino purity rule is essential.** Arduino-cli's auto-prototype generation breaks on project-defined types. The fix is to keep the `.ino` file empty of everything except `setup()` and `loop()`.

2. **Counterfeit RFID dies are real.** The RC522 module on the bench reports version `0x82` instead of the documented `0x00` or `0xFF`. Treating unknown versions as faults produced four false failures during bring-up.

3. **Matrix `begin()` is not a test.** `Arduino_LED_Matrix::begin()` returns 1 unconditionally. Software cannot verify that any light actually came out. Physical confirmation is required.

4. **Transistor pinout varies by manufacturer.** With the flat face toward you: PN2222A is E-B-C, but P2N2222A and BC547 are C-B-E. Always verify with a multimeter diode test before powering on.

5. **Time must be injectable.** Reading the system clock directly makes tests slow and non-deterministic. Injecting a `monotonic_ms()` function that tests can monkeypatch is essential for fast, reliable testing.

### 11.3 Limitations

1. **No cloud connectivity** — cannot support remote tournaments or centralised analytics
2. **Single-board only** — no multi-console networking for large events
3. **JSON-based corpus** — no GUI for content management
4. **No native mobile app** — web-based only (requires browser)
5. **Fixed hardware** — cannot scale to larger displays or more complex I/O without redesign

---

## 12. Conclusion and Future Work

### 12.1 Conclusion

PhishTap Lite demonstrates that effective cybersecurity education can be delivered through a physical computing platform while maintaining strict privacy standards and operating within embedded system constraints. The project achieves:

- **Engagement** through arcade-style gameplay with physical RFID interaction
- **Education** through 40 realistic multi-channel phishing messages with detailed explanations
- **Privacy** through HMAC-SHA256 pseudonymisation with zero plaintext UID storage
- **Quality** through 374 automated tests across 5 verification gates
- **Portability** through offline operation on a single board with no internet dependency

The dual-processor architecture of the Arduino UNO Q enables a clean separation between real-time I/O handling and application logic, resulting in responsive gameplay and maintainable code. The enforced layering rule ensures that the core game logic remains testable on a laptop without hardware, accelerating development and improving reliability.

### 12.2 Future Work

#### Stage 6: Button Input
Add physical buttons on D2 and D4 for players who prefer not to use their phones, enabling direct console-based gameplay.

#### Stage 7: Multi-Board Networking
Implement tournament mode where multiple consoles share a leaderboard over local Wi-Fi.

#### Stage 8: Content Management System
Build a web-based GUI for creating and editing corpus items, replacing the current JSON-based workflow.

#### Stage 9: Analytics Dashboard
Enhance the admin page with cohort-level analytics, identifying which phishing cues students struggle with most.

#### Stage 10: Mobile Native App
Develop a Progressive Web App (PWA) or native mobile app for improved offline gameplay experience.

---

## 13. References

1. Deterding, S., Dixon, D., Khaled, R., & Nacke, L. (2011). From game design elements to gamefulness: defining "gamification". *Proceedings of the 15th International Academic MindTrek Conference*.

2. Hamari, J., Koivisto, J., & Salmela-Aro, K. (2016). A gamified learning experience in mathematics. *Computers in Human Behavior*, 41, 332-341.

3. Gwenhure, A. K., et al. (2024). Gamification of Cybersecurity Awareness for Non-IT Professionals. *International Journal of Serious Games*, 11(3).

4. European Union Agency for Cybersecurity (ENISA). (2023). *Threat Landscape for Supply Chain Attacks*.

5. FBI Internet Crime Complaint Center (IC3). (2023). *Annual Report*.

6. European Commission. (2024). *Gamification and Serious Games for Cybersecurity Awareness*. EyesOnCS Project.

7. IEEE. (2022). Teaching and Learning Cybersecurity Awareness with Gamification in Smaller Universities and Colleges. *Frontiers in Education Conference*.

8. Pahlavanpour, O., et al. (2024). A systematic mapping study on gamification within information security awareness. *PMC*.

9. Arduino. (2025). *Arduino UNO Q Documentation*. https://docs.arduino.cc/hardware/uno-q

10. STMicroelectronics. (2023). *STM32U585 Reference Manual*.

11. Qualcomm. (2023). *QRB2210 Datasheet*.

12. NXP. (2016). *MFRC522 Standard Performance MIFARE and NTAG Frontend*.

13. European Commission. (2018). *General Data Protection Regulation (GDPR)*. Article 25: Data Protection by Design and by Default.

---

## 14. Appendices

### Appendix A: Project Timeline

| Stage | Description | Status |
|-------|-------------|--------|
| Stage 0 | Architecture documentation | ✅ Complete |
| Stage 1a | Hardware self-test sketch | ✅ Verified on hardware |
| Stage 1b | Production firmware | ✅ Verified on hardware |
| Stage 2 | Python backend | ✅ Complete |
| Stage 3 | Frontend (5 pages) | ✅ Complete |
| Stage 4 | Corpus expansion (40 items) | ✅ Complete |
| Stage 5 | Documentation suite (9 files) | ✅ Complete |
| Stage 6 | Button input | Planned |

### Appendix B: Bill of Materials

| Item | Spec | Cost (Approx.) |
|------|------|----------------|
| Arduino UNO Q | 4 GB RAM, 32 GB eMMC | $80-100 |
| RC522 RFID module | 13.56 MHz, SPI, 3.3V | $3-5 |
| MIFARE Classic 1K cards | 4-byte UID, white | $0.50 each |
| TMB12A05 buzzer | Active, 5V, electromagnetic | $1-2 |
| 2N2222A or BC547 | NPN transistor, TO-92 | $0.10 |
| 3× LED | Red, 3mm or 5mm | $0.05 each |
| 3× 150 Ω resistor | For LEDs | $0.02 each |
| 1 kΩ, 10 kΩ resistors | For buzzer circuit | $0.02 each |
| 1N4007 diode | Flyback protection | $0.05 |
| 5V / 3A USB-C charger | Power supply | $10-15 |
| **Total** | | **~$100-130** |

### Appendix C: Verification Gate Scripts

| Script | Purpose | Exit Codes |
|--------|---------|------------|
| `tools/syntax_check.sh` | C++ syntax validation | 0=pass, 1=fail |
| `tools/no_write_check.sh` | RFID write policy | 0=clean, 1=violation |
| `tools/host_gate.sh` | Firmware unit tests | 0=pass, 1=fail |
| `tools/web_gate.py` | Frontend safety | 0=pass, 1=fail |
| `tools/board_compile.sh` | Real hardware compile | 0=pass, 1=fail |
| `tools/deploy_to_board.sh` | Full deployment | Runs gates 1-3 first |

### Appendix D: API Endpoints

| Method | Endpoint | Purpose |
|--------|----------|---------|
| POST | `/api/consent` | Record player consent |
| POST | `/api/join` | Join game with PIN |
| POST | `/api/answer` | Submit round answer |
| GET | `/api/status` | Console status |
| GET | `/api/leaderboard` | Top scores |
| GET | `/api/review/{token}` | Post-game review |
| DELETE | `/api/players/{id}` | Delete player data |
| GET | `/api/admin/stats` | Admin analytics |

### Appendix E: Glossary

| Term | Definition |
|------|-----------|
| **MCU** | Microcontroller Unit (STM32U585) |
| **MPU** | Microprocessor Unit (Qualcomm QRB2210) |
| **HMAC** | Hash-based Message Authentication Code |
| **Pseudonymisation** | Replacing identifying data with artificial identifiers |
| **Bridge** | UART communication channel between MCU and MPU |
| **Ring buffer** | Fixed-size circular data structure for event queuing |
| **WAL** | Write-Ahead Logging (SQLite journal mode) |
| **NFKC** | Unicode Normalisation Form Compatibility Composition |
| **Defanged URL** | URL modified to prevent accidental navigation (e.g., hxxps://) |

---

<div align="center">

**PhishTap Lite — Making cybersecurity education fun, one tap at a time.**

*Built with ❤️ for cybersecurity education*

</div>
