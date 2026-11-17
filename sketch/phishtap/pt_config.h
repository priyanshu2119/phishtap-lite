/*
 * PhishTap Lite — production firmware (Stage 1b)
 *
 * Every tunable constant in one place.
 *
 * WHY: these numbers are the ones that will need adjusting once the game is
 * played by real people in a real room — how often to poll the reader, how long
 * a cooldown feels right, how big the event ring must be before Python drops
 * events. Scattering them through the engines makes each adjustment a hunt and
 * each hunt a chance to change the wrong one.
 *
 * Every value below carries its reasoning and, where relevant, the measured
 * fact that justifies it. A magic number with no stated reason is a number
 * nobody dares change later.
 */

#ifndef PT_CONFIG_H
#define PT_CONFIG_H

/* ── Protocol identity ────────────────────────────────────────────────────
 * Reported by pt.get_status so Python can refuse to talk to firmware whose
 * protocol it does not understand. Bump PT_PROTO_VERSION on any change to the
 * shape of a payload, not merely to its contents.
 */
#define PT_PROTO_VERSION   1
#define PT_FW_VERSION      "1b.1"

/* ── Event ring ───────────────────────────────────────────────────────────
 * 16 slots, per docs/ARCHITECTURE.md §4. Sized for the worst realistic burst:
 * a heartbeat every second plus a tap, a removal and a timeout close together
 * is four events, and Python polls roughly every 100 ms, so it drains far
 * faster than anything can fill it. 16 gives several seconds of headroom if
 * Python stalls, which is the actual failure mode worth surviving.
 *
 * Must be a power of two only if the modulo is replaced by a mask; the code
 * uses %, so any value works.
 */
#define PT_EVENT_RING_SIZE 16

/* Longest UID this project will encode. MIFARE 1K is 4 bytes (8 hex chars),
 * but ISO 14443 allows 7 and 10 byte UIDs and the reader reports whatever the
 * card carries, so the buffer is sized for the 10 byte maximum. Sizing it for 4
 * would truncate silently on a 7 byte card rather than failing loudly. */
#define PT_UID_HEX_MAX     20

/* ── RFID polling ─────────────────────────────────────────────────────────
 * PT_RFID_POLL_MS is how often SEEK sends REQA while waiting for a tap.
 *
 * 20 ms is 50 Hz. That is fast enough that a card waved at the reader is never
 * missed — a human tap keeps the card in the field for hundreds of ms — and
 * slow enough that the reader is not being hammered. Each REQA is a handful of
 * SPI transactions at 4 MHz, well under a millisecond, so at 20 ms the reader
 * work occupies a few percent of the loop and leaves the matrix and buzzer
 * engines plenty of time.
 *
 * Polling faster buys nothing perceptible and increases the chance of colliding
 * with a card that is still settling in the field.
 */
#define PT_RFID_POLL_MS           20

/* How often PRESENT_WATCH probes with WUPA to notice a card being taken away.
 * 100 ms feels instant to a person and costs two SPI transactions per probe
 * (WUPA, then HaltA to restore the HALT state so the next probe is valid). */
#define PT_RFID_PRESENT_PROBE_MS  100

/* Same-card cooldown, per docs/ARCHITECTURE.md §6. A card left sitting on the
 * reader must not create a second session. 3 s is long enough to cover a person
 * who taps, leaves the card down while they look at the screen, then picks it
 * up, and short enough that deliberately tapping again soon works. */
#define PT_CARD_COOLDOWN_MS       3000

/* How many consecutive failed SEEK polls before the reader is considered faulty
 * and an hw_fault is queued. Not a timeout in ms, because a card-free reader
 * legitimately times out on every single poll — this counts INIT failures and
 * read errors, not absence of cards. */
#define PT_RFID_FAULT_THRESHOLD   8

/* ── Display and feedback ─────────────────────────────────────────────────
 * Matrix animation frame interval, per docs/ARCHITECTURE.md §5. 80 ms is
 * 12.5 fps, which reads as smooth motion on a 13-column scroller and is slow
 * enough that the panel's own refresh is never the bottleneck.
 */
#define PT_MATRIX_TICK_MS         80

/* Countdown tick cadence bounds. The architecture specifies
 * clamp(remaining/8, 60 ms, 500 ms), so the buzzer accelerates as time runs
 * out. The bounds exist because an unbounded division would tick every 125 ms
 * at 1 s remaining and every 1.75 s at 14 s, and both extremes feel wrong: too
 * fast is a buzzsaw, too slow is no feedback at all. */
#define PT_COUNTDOWN_TICK_MIN_MS  60
#define PT_COUNTDOWN_TICK_MAX_MS  500

/* ── Bridge liveness ──────────────────────────────────────────────────────
 * The MCU queues a heartbeat this often. Python polls at roughly 100 ms, so a
 * 1 s interval means about ten polls per heartbeat — frequent enough to detect
 * loss quickly, sparse enough not to fill the ring.
 */
#define PT_HEARTBEAT_MS           1000

/* If no RPC of any kind arrives from Python for this long, the MCU concludes
 * the link is gone, returns to idle and shows the link-lost animation.
 * 15 s is fifteen missed heartbeats' worth of silence, which cannot happen by
 * accident but happens immediately if the Python side crashes or the app is
 * stopped. Per docs/ARCHITECTURE.md §4. */
#define PT_LINK_LOSS_MS           15000

/* ── Response buffers ─────────────────────────────────────────────────────
 * PT_JSON_SMALL covers every response except pt.get_events: the largest of
 * those is pt.get_status at roughly 150 bytes, so 256 leaves room to grow.
 *
 * PT_JSON_EVENTS must hold a full ring. One heartbeat event serialises to about
 * 70 bytes and one card_tap to about 110, so sixteen of the worst case is about
 * 1760 bytes plus the envelope. 2048 fits that with margin. If it ever does not
 * fit, ptJsonFinish returns 0 and the handler replies with an explicit error
 * rather than sending a truncated document that Python cannot parse.
 */
#define PT_JSON_SMALL             256
#define PT_JSON_EVENTS            2048

/* ── Boot selftest ────────────────────────────────────────────────────────
 * The production sketch runs a short selftest at boot, unlike hw_selftest which
 * is entirely a diagnostic. It must be quick: a student waiting at a tabletop
 * console does not want to watch a minute of lamp tests. Each subsystem gets a
 * brief visible confirmation and the results are reported through pt.selftest.
 */
#define PT_BOOT_SELFTEST_MS       1200

/* ── Round limits ─────────────────────────────────────────────────────────
 * Guards against a malformed pt.start_round. The architecture's formula is
 * max(4, 14 - 0.8 * round_index) seconds, so 4000 ms is the shortest legal
 * round and 14000 ms the longest. A limit outside that range means Python sent
 * something wrong, and clamping it silently would hide the bug, so the handler
 * rejects it instead. These bounds are the definition of "wrong".
 */
#define PT_ROUND_LIMIT_MIN_MS     4000
#define PT_ROUND_LIMIT_MAX_MS     14000

/* Lives are fixed at three by the game rules. The MCU only displays them, but
 * it validates the value so a corrupt set_lives cannot light more LEDs than
 * exist or write out of bounds on the pin array. */
#define PT_MAX_LIVES              3

#endif /* PT_CONFIG_H */
