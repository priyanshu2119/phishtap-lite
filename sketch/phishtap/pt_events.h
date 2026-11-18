/*
 * PhishTap Lite — production firmware (Stage 1b)
 *
 * Statically allocated event ring buffer, plus the fault codes and the JSON
 * serialisation for pt.get_events.
 *
 * WHY A RING AND NOT A QUEUE OR A CALLBACK
 * The MCU must never block on Python. docs/ARCHITECTURE.md §4 fixes that rule,
 * and it is load-bearing: the bridge runs its safe handlers inside __loopHook,
 * which the core calls as `for(;;) { loop(); __loopHook(); }` (verified in
 * cores/arduino/main.cpp). So a handler that waits for anything stalls loop(),
 * which stalls the RFID poll, the countdown and every animation at once.
 *
 * Instead the MCU records events into a fixed ring and Python drains it at its
 * own pace. If Python falls behind, the oldest events are dropped and an
 * overflow flag says so — a visible, recoverable degradation rather than a hang.
 *
 * WHY STATIC AND POD
 * No heap, ever. The ring is a fixed array of plain structs, allocated once at
 * startup. docs/ARCHITECTURE.md R10 names String heap fragmentation as a risk on
 * the MCU; the answer is to not allocate. sizeof(PtEvent) * 16 is a few hundred
 * bytes against 786 KB of SRAM.
 *
 * WHY NO LOCKS
 * The core's loop is single-threaded with respect to safe handlers: loop() runs
 * to completion, then __loopHook() runs them, then loop() again. Producers
 * (loop) and consumers (handlers) therefore never run concurrently, and adding a
 * mutex would only add a way to deadlock. This is the single most important
 * consequence of the verified loop structure, and it is why this file contains
 * no synchronisation at all.
 *
 * NO Arduino.h DEPENDENCY, DELIBERATELY
 * Timestamps are passed IN by the caller rather than read from millis() here.
 * That keeps this module compilable under plain g++, so tools/test_pt_json.cpp
 * can exercise the sequence-number and overflow arithmetic with real assertions
 * before it runs on hardware. Ring buffer bookkeeping is exactly where an
 * off-by-one hides, and an off-by-one here means silently lost card taps.
 */

#ifndef PT_EVENTS_H
#define PT_EVENTS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pt_config.h"
#include "pt_json.h"

/* ── Fault codes ──────────────────────────────────────────────────────────
 * The MCU-relevant subset of the error codes in docs/ARCHITECTURE.md §4. The
 * rest (E_TOKEN, E_PIN, E_DB and so on) are server-side only and never appear
 * in an MCU event, so defining them here would just invite confusion about
 * which side owns which failure.
 */
typedef enum : uint8_t {
  PT_FAULT_NONE = 0,
  PT_FAULT_RFID_INIT,    /* PCD_Init failed, or the version read as 0x00/0xFF */
  PT_FAULT_RFID_READ,    /* repeated read failures with a card believed present */
  PT_FAULT_MATRIX,       /* matrixBegin failed */
  PT_FAULT_BRIDGE,       /* Bridge.begin failed, or a handler could not bind */
  PT_FAULT_INTERNAL      /* anything else that should not happen */
} PtFaultCode;

inline const char* ptFaultName(PtFaultCode c) {
  switch (c) {
    case PT_FAULT_RFID_INIT:  return "E_RFID_INIT";
    case PT_FAULT_RFID_READ:  return "E_RFID_READ";
    case PT_FAULT_MATRIX:     return "E_MATRIX";
    case PT_FAULT_BRIDGE:     return "E_BRIDGE";
    case PT_FAULT_INTERNAL:   return "E_INTERNAL";
    default:                  return "E_NONE";
  }
}

/* ── Event types ──────────────────────────────────────────────────────────
 * Names match the wire strings in docs/ARCHITECTURE.md §4 exactly.
 */
typedef enum : uint8_t {
  PT_EV_NONE = 0,
  PT_EV_CARD_TAP,
  PT_EV_CARD_REMOVED,
  PT_EV_ROUND_TIMEOUT,
  PT_EV_BUTTON,
  PT_EV_HW_FAULT,
  PT_EV_HEARTBEAT
} PtEventType;

inline const char* ptEventTypeName(PtEventType t) {
  switch (t) {
    case PT_EV_CARD_TAP:      return "card_tap";
    case PT_EV_CARD_REMOVED:  return "card_removed";
    case PT_EV_ROUND_TIMEOUT: return "round_timeout";
    case PT_EV_BUTTON:        return "button";
    case PT_EV_HW_FAULT:      return "hw_fault";
    case PT_EV_HEARTBEAT:     return "heartbeat";
    default:                  return "none";
  }
}

/* ── The event record ─────────────────────────────────────────────────────
 * One POD struct covering every event type, with the type deciding which fields
 * are meaningful. A union would save a few bytes but would make the
 * serialisation code depend on getting the active member right, and the whole
 * ring is only a few hundred bytes. Clarity wins.
 *
 * Field meanings by type:
 *   card_tap       uid_hex, uid_len, t_ms
 *   card_removed   t_ms only
 *   round_timeout  aux16 = round number
 *   button         aux16 = button id, aux32 = micros() timestamp
 *   hw_fault       fault = code
 *   heartbeat      aux32 = uptime_ms
 */
typedef struct {
  uint32_t     seq;                          /* assigned by the ring, never 0 */
  uint32_t     t_ms;                         /* millis() when queued          */
  PtEventType  type;
  uint8_t      uid_len;
  uint8_t      fault;                        /* PtFaultCode                   */
  uint16_t     aux16;
  uint32_t     aux32;
  char         uid_hex[PT_UID_HEX_MAX + 1];
} PtEvent;

/* ── The ring ─────────────────────────────────────────────────────────────
 * Sequence numbers are monotonic and start at 1, so 0 unambiguously means
 * "before the first event" and is the natural initial cursor for Python.
 *
 * EMPTY IS REPRESENTED AS newestSeq < oldestSeq, not as a zero count. That
 * choice is what makes the arithmetic below free of special cases: a fresh ring
 * is {oldest=1, newest=0}, the first push lands on seq 1 without adjusting
 * oldest, and acking everything leaves {oldest=n+1, newest=n} which is empty
 * again by the same rule. Deriving emptiness from a separate counter instead
 * would need that counter kept in step with both ends, which is one more thing
 * to get wrong.
 */
typedef struct {
  PtEvent  slots[PT_EVENT_RING_SIZE];
  uint32_t oldestSeq;
  uint32_t newestSeq;
  uint32_t overflows;      /* total events dropped since boot */
  uint32_t totalPushed;    /* total events queued since boot  */
} PtEventRing;

inline void ptRingInit(PtEventRing* r) {
  memset(r, 0, sizeof(*r));
  r->oldestSeq = 1;
  r->newestSeq = 0;        /* newest < oldest => empty */
}

inline uint32_t ptRingLen(const PtEventRing* r) {
  return (r->newestSeq >= r->oldestSeq) ? (r->newestSeq - r->oldestSeq + 1) : 0;
}

inline bool ptRingEmpty(const PtEventRing* r) {
  return r->newestSeq < r->oldestSeq;
}

/* Append an event. The caller fills in everything except seq, which the ring
 * assigns. Returns the seq used.
 *
 * On a full ring the OLDEST event is discarded. Dropping the oldest rather than
 * refusing the newest is the right trade here: a stale heartbeat is worthless,
 * while the card tap happening right now is the event the whole system exists to
 * capture. The overflow counter makes the loss visible either way.
 */
inline uint32_t ptRingPush(PtEventRing* r, PtEvent ev) {
  uint32_t newSeq = r->newestSeq + 1;   /* 0 + 1 = 1 on a fresh ring */
  ev.seq = newSeq;

  r->slots[newSeq % PT_EVENT_RING_SIZE] = ev;
  r->newestSeq   = newSeq;
  r->totalPushed += 1;

  uint32_t len = ptRingLen(r);
  if (len > PT_EVENT_RING_SIZE) {
    uint32_t dropped = len - PT_EVENT_RING_SIZE;
    r->oldestSeq += dropped;
    r->overflows += dropped;
  }
  return newSeq;
}

/* Python has processed everything up to and including `seq`, so those slots can
 * be reclaimed. Acking a seq the ring never held is ignored rather than treated
 * as an error: after a restart Python may ack a cursor from the previous boot,
 * and refusing that would wedge the link until the next full resync.
 *
 * Acking is an optimisation, not a correctness requirement. Even with no acks at
 * all the ring stays bounded, because push() drops the oldest on overflow.
 */
inline void ptRingAck(PtEventRing* r, uint32_t seq) {
  if (seq == 0) return;
  if (seq + 1 > r->oldestSeq) {
    r->oldestSeq = seq + 1;
    /* Never let the ack run past the newest event, which would invent a
     * negative-length ring and make every subsequent length calculation wrong. */
    if (r->oldestSeq > r->newestSeq + 1) r->oldestSeq = r->newestSeq + 1;
  }
}

/* ── Producers ────────────────────────────────────────────────────────────
 * One small function per event type, so a caller cannot accidentally put a
 * round number in the button-id field. Each takes t_ms explicitly; see the
 * header comment for why this module does not call millis() itself.
 */

inline uint32_t ptRingPushCardTap(PtEventRing* r, uint32_t t_ms,
                                  const char* uid_hex, uint8_t uid_len) {
  PtEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type    = PT_EV_CARD_TAP;
  ev.t_ms    = t_ms;
  ev.uid_len = uid_len;
  if (uid_hex != NULL) {
    /* Bounded copy: a card reporting an unexpected UID length must truncate
     * here rather than overrun the field. */
    strncpy(ev.uid_hex, uid_hex, PT_UID_HEX_MAX);
    ev.uid_hex[PT_UID_HEX_MAX] = '\0';
  }
  return ptRingPush(r, ev);
}

inline uint32_t ptRingPushCardRemoved(PtEventRing* r, uint32_t t_ms) {
  PtEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = PT_EV_CARD_REMOVED;
  ev.t_ms = t_ms;
  return ptRingPush(r, ev);
}

inline uint32_t ptRingPushRoundTimeout(PtEventRing* r, uint32_t t_ms,
                                       uint16_t round) {
  PtEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type  = PT_EV_ROUND_TIMEOUT;
  ev.t_ms  = t_ms;
  ev.aux16 = round;
  return ptRingPush(r, ev);
}

inline uint32_t ptRingPushButton(PtEventRing* r, uint32_t t_ms,
                                 uint16_t id, uint32_t t_us) {
  PtEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type  = PT_EV_BUTTON;
  ev.t_ms  = t_ms;
  ev.aux16 = id;
  ev.aux32 = t_us;
  return ptRingPush(r, ev);
}

inline uint32_t ptRingPushHwFault(PtEventRing* r, uint32_t t_ms,
                                  PtFaultCode code) {
  PtEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type  = PT_EV_HW_FAULT;
  ev.t_ms  = t_ms;
  ev.fault = (uint8_t)code;
  return ptRingPush(r, ev);
}

inline uint32_t ptRingPushHeartbeat(PtEventRing* r, uint32_t t_ms) {
  PtEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type  = PT_EV_HEARTBEAT;
  ev.t_ms  = t_ms;
  ev.aux32 = t_ms;      /* uptime_ms; millis() is uptime on this core */
  return ptRingPush(r, ev);
}

/* ── Serialisation ────────────────────────────────────────────────────────
 * Builds the pt.get_events response:
 *   {"events":[{"seq":N,"t_ms":N,"type":"...","data":{...}},...],
 *    "oldest_seq":N,"overflow":bool}
 *
 * `since` is Python's cursor: only events with seq > since are included.
 *
 * overflowOut reports whether the caller's cursor fell behind the oldest
 * retained event, meaning some events were dropped before it could read them.
 * That is distinct from r->overflows, which is a cumulative boot-long counter;
 * this flag answers "did YOU miss something on THIS poll", which is what Python
 * needs in order to decide to resync and log.
 *
 * `countOut`, if non-NULL, receives the number of events actually emitted. The
 * caller needs this to decide whether a poll is worth logging: Python polls
 * roughly ten times a second and almost every poll returns nothing, so printing
 * each one buries the lines that matter.
 *
 * Returns the JSON length, or 0 if it did not fit. A 0 return must be treated as
 * an error by the caller and answered with an explicit failure, never sent as an
 * empty or truncated document.
 */
inline size_t ptEventsWriteJson(const PtEventRing* r, uint32_t since,
                                char* buf, size_t cap, bool* overflowOut,
                                uint32_t* countOut = NULL) {
  PtJsonWriter w;
  ptJsonInit(&w, buf, cap);

  bool missed = false;
  uint32_t emitted = 0;
  uint32_t start = since + 1;
  if (!ptRingEmpty(r) && start < r->oldestSeq) {
    /* The caller asked for events older than the ring still holds. */
    if (since != 0) missed = true;
    start = r->oldestSeq;
  }

  ptJsonObjBegin(&w);
  ptJsonKey(&w, "events");
  ptJsonArrBegin(&w);

  if (!ptRingEmpty(r)) {
    for (uint32_t seq = start; seq <= r->newestSeq; ++seq) {
      const PtEvent* e = &r->slots[seq % PT_EVENT_RING_SIZE];
      /* Defensive: a slot can only hold a different seq if the ring wrapped
       * between computing start and reading it, which cannot happen in a
       * single-threaded loop. Skipping rather than emitting keeps a corrupt
       * ring from producing a payload with duplicate seq numbers, which would
       * confuse Python's cursor far more than a missing event would. */
      if (e->seq != seq) continue;

      emitted++;
      ptJsonObjBegin(&w);
      ptJsonKVUint(&w, "seq", (unsigned long)seq);
      ptJsonKVUint(&w, "t_ms", (unsigned long)e->t_ms);
      ptJsonKVStr(&w, "type", ptEventTypeName(e->type));
      ptJsonKey(&w, "data");
      ptJsonObjBegin(&w);
      switch (e->type) {
        case PT_EV_CARD_TAP:
          ptJsonKVStr(&w, "uid_hex", e->uid_hex);
          ptJsonKVInt(&w, "uid_len", (long)e->uid_len);
          ptJsonKVUint(&w, "t_ms", (unsigned long)e->t_ms);
          break;
        case PT_EV_CARD_REMOVED:
          /* No payload. The empty object is still emitted so Python can index
           * data unconditionally instead of special-casing this type. */
          break;
        case PT_EV_ROUND_TIMEOUT:
          ptJsonKVInt(&w, "round", (long)e->aux16);
          break;
        case PT_EV_BUTTON:
          ptJsonKVInt(&w, "id", (long)e->aux16);
          ptJsonKVUint(&w, "t_us", (unsigned long)e->aux32);
          break;
        case PT_EV_HW_FAULT:
          ptJsonKVStr(&w, "code", ptFaultName((PtFaultCode)e->fault));
          break;
        case PT_EV_HEARTBEAT:
          ptJsonKVUint(&w, "uptime_ms", (unsigned long)e->aux32);
          break;
        default:
          break;
      }
      ptJsonObjEnd(&w);   /* data */
      ptJsonObjEnd(&w);   /* event */
    }
  }

  ptJsonArrEnd(&w);
  ptJsonKVUint(&w, "oldest_seq",
               (unsigned long)(ptRingEmpty(r) ? 0 : r->oldestSeq));
  ptJsonKVBool(&w, "overflow", missed);
  ptJsonObjEnd(&w);

  size_t n = ptJsonFinish(&w);
  if (overflowOut != NULL) *overflowOut = missed;
  if (countOut != NULL) *countOut = emitted;
  return n;
}

#endif /* PT_EVENTS_H */
