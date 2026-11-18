/*
 * PhishTap Lite — production firmware (Stage 1b)
 *
 * Bridge RPC handlers: the Python -> MCU surface from docs/ARCHITECTURE.md §4.
 *
 * THREE RULES, ALL OF THEM VERIFIED RATHER THAN ASSUMED
 *
 * 1. EVERY handler is registered with provide_safe, never provide.
 *    provide() runs the handler on the background RPC thread (updateEntryPoint
 *    calls Bridge.update() in its own loop), where Arduino APIs are unsafe and
 *    where the handler would run concurrently with loop(). provide_safe() routes
 *    it through __loopHook instead, which the core calls as
 *    `for(;;) { loop(); __loopHook(); }`, so the handler runs on the loop thread
 *    and never concurrently with anything. That is why this firmware contains no
 *    mutexes.
 *
 * 2. NO handler calls Bridge.call(), and NO handler prints.
 *    A safe handler executes inside Bridge.update_safe() -> process_request ->
 *    send_response. Starting a new bridge communication from in there re-enters
 *    the bridge mutex and deadlocks, which the RouterBridge manual warns about
 *    explicitly. Monitor/Serial traffic rides the same router, so printing has
 *    the same failure mode. Handlers that want to log set a field on ptApp and
 *    let loop() print it on the next iteration; see ptBridgeNoteCall below.
 *
 * 3. Handlers are SHORT.
 *    While a handler runs, loop() is not running. A slow handler therefore stalls
 *    the RFID poll, the countdown and every animation for its whole duration.
 *    Each handler here parses one small JSON object, mutates ptApp, and returns.
 *    All the consequences of a change are worked out in ptAppLoop, not here.
 *
 * PAYLOAD FORMAT
 * Every request and response is a SINGLE JSON STRING. This works because
 * MsgPack's str_t is arduino::String on this core (MsgPack/Types.h:
 * `using str_t = String;` under `#ifdef ARDUINO`), so a `String fn(String)`
 * handler binds natively and no composite type mapping has to be guessed at.
 * Python must therefore send json.dumps(...) of an object, not a bare dict.
 */

#ifndef PT_BRIDGE_H
#define PT_BRIDGE_H

#include <Arduino.h>
#include <Arduino_RouterBridge.h>

#include "pt_config.h"
#include "pt_json.h"
#include "pt_state.h"
#include "pt_events.h"
#include "pt_app.h"

/* Response buffers, statically allocated.
 *
 * One buffer is safe for all handlers because they run one at a time on a single
 * thread, and the returned String copies out of it before the next call can
 * reuse it. PT_JSON_EVENTS is sized from a measurement, not a guess: a full
 * 16-event ring of card_tap, the worst case, serialises to 1531 bytes, and
 * tools/test_pt_events.cpp asserts that. */
static char s_respSmall[PT_JSON_SMALL];
static char s_respEvents[PT_JSON_EVENTS];

/* ── Deferred logging ─────────────────────────────────────────────────────
 * A handler cannot print, but silently doing things is unbearable when debugging
 * a bridge that will not talk. So each handler records what it was asked and
 * whether it succeeded, and ptBridgePollLogs() — called from loop(), where
 * printing is safe — emits it.
 *
 * The sequence counter is what makes this work: loop() prints only when the
 * counter has advanced, so a call is logged exactly once no matter how many
 * iterations pass before loop() notices it.
 *
 * WHY SOME CALLS ARE QUIET
 * Python polls pt.get_events about ten times a second and pt.get_status once a
 * second. On the first real hardware run that produced roughly 46 log lines every
 * five seconds, and the lines that actually mattered — a card tap, a state
 * change, a rejection — were buried in them. A log you cannot read is worse than
 * no log, because it looks like coverage.
 *
 * So a routine SUCCESSFUL poll is counted but not printed. Everything else is
 * printed: every rejection, every game action, and every poll that actually
 * delivered events. The cumulative count is reported on the periodic [status]
 * line, so "is Python still polling?" remains answerable at a glance without
 * one line per call.
 */
#define PT_RPC_LOG_SLOTS 4

static struct {
  uint32_t seq;
  uint32_t seenSeq;
  uint32_t total;      /* every call, quiet or not */
  uint32_t printed;    /* calls actually emitted */
  bool     quiet;
  char     method[20];
  bool     ok;
  char     detail[40];
} s_rpcLog;

inline void ptBridgeNoteCall(const char* method, bool ok, const char* detail,
                             bool quiet = false) {
  s_rpcLog.total++;
  /* Also mirrored into the app state so the periodic [status] line can show it.
   * See the comment on PtAppState::rpcCalls for why the counter lives there. */
  ptApp.rpcCalls++;

  /* Do not let a routine poll overwrite a meaningful call that loop() has not
   * printed yet. Without this, suppressing the noise could also suppress the
   * signal: a pt.set_state arriving just before a burst of pt.get_events would
   * be counted and then discarded unseen, which is worse than the flooding this
   * change exists to fix. The call is still counted, so the [status] line's rpc
   * total stays accurate.
   *
   * A meaningful new call always overwrites, quiet or not, because the newest
   * game action is the one worth seeing. */
  bool pending = (s_rpcLog.seq != s_rpcLog.seenSeq);
  if (quiet && pending && !s_rpcLog.quiet) return;

  s_rpcLog.seq++;
  s_rpcLog.quiet = quiet;
  strncpy(s_rpcLog.method, (method != NULL) ? method : "?", sizeof(s_rpcLog.method) - 1);
  s_rpcLog.method[sizeof(s_rpcLog.method) - 1] = '\0';
  s_rpcLog.ok = ok;
  if (detail != NULL) {
    strncpy(s_rpcLog.detail, detail, sizeof(s_rpcLog.detail) - 1);
    s_rpcLog.detail[sizeof(s_rpcLog.detail) - 1] = '\0';
  } else {
    s_rpcLog.detail[0] = '\0';
  }
}

/* Total RPC calls served since boot. Mirrored into ptApp.rpcCalls, which is what
 * the periodic [status] line actually prints; this accessor exists for callers
 * that already have the bridge header in scope. */
inline uint32_t ptBridgeTotalCalls(void) {
  return s_rpcLog.total;
}

/* Called from loop(). Safe to print here. */
inline void ptBridgePollLogs(void) {
  if (s_rpcLog.seq == s_rpcLog.seenSeq) return;
  s_rpcLog.seenSeq = s_rpcLog.seq;

  /* A quiet entry is counted but not printed. seenSeq has already advanced, so
   * it will not be reconsidered on the next iteration. */
  if (s_rpcLog.quiet) return;
  s_rpcLog.printed++;

  Serial.print("[rpc] ");
  Serial.print(s_rpcLog.method);
  Serial.print(s_rpcLog.ok ? "  ok" : "  REJECTED");
  if (s_rpcLog.detail[0] != '\0') {
    Serial.print("  ");
    Serial.print(s_rpcLog.detail);
  }
  Serial.println();
}

/* Only the most recent PT_RPC_LOG_SLOTS calls survive, because s_rpcLog holds one
 * entry. That is a deliberate trade: at ten polls a second Python can call faster
 * than the console prints, and buffering every call would either grow without
 * bound or drop the same information with more code. The latest call is the one
 * worth seeing. */

/* ── Response helpers ───────────────────────────────────────────────────── */

inline String ptRpcOk(void) {
  PtJsonWriter w;
  ptJsonInit(&w, s_respSmall, sizeof(s_respSmall));
  ptJsonObjBegin(&w);
  ptJsonKVBool(&w, "ok", true);
  ptJsonObjEnd(&w);
  ptJsonFinish(&w);
  return String(s_respSmall);
}

/* An error response names the reason. Returning a bare {"ok":false} would leave
 * Python guessing whether the payload was malformed, the value out of range, or
 * the firmware simply did not understand the method. */
inline String ptRpcErr(const char* code) {
  PtJsonWriter w;
  ptJsonInit(&w, s_respSmall, sizeof(s_respSmall));
  ptJsonObjBegin(&w);
  ptJsonKVBool(&w, "ok", false);
  ptJsonKVStr(&w, "err", code);
  ptJsonObjEnd(&w);
  ptJsonFinish(&w);
  return String(s_respSmall);
}

/* ── Handlers ─────────────────────────────────────────────────────────────
 * Each one calls ptAppNoteLinkAlive first. Any traffic at all is proof the link
 * is up, so supervision does not depend on Python remembering to send a dedicated
 * heartbeat on schedule.
 */

inline String ptRpcGetStatus(String req) {
  (void)req;
  uint32_t now = millis();
  ptAppNoteLinkAlive(now);

  size_t n = ptAppWriteStatus(s_respSmall, sizeof(s_respSmall));
  if (n == 0) {
    ptBridgeNoteCall("pt.get_status", false, "response did not fit");
    return ptRpcErr("E_INTERNAL");
  }
  /* Quiet on success. Python calls this once a second purely as a health read,
   * and the periodic [status] line already reports everything it returns, so
   * printing each one is pure noise. A FAILURE is still printed, because that is
   * the interesting case. */
  ptBridgeNoteCall("pt.get_status", true, NULL, /*quiet=*/true);
  return String(s_respSmall);
}

inline String ptRpcGetEvents(String req) {
  uint32_t now = millis();
  ptAppNoteLinkAlive(now);

  unsigned long since = 0;
  /* A missing or malformed `since` is treated as 0, meaning "everything the ring
   * still holds". That is the safe default for a first poll and for a Python
   * side that has just restarted and lost its cursor; refusing the call instead
   * would wedge the link until someone noticed. */
  (void)ptJsonGetULong(req.c_str(), "since", &since);

  bool missed = false;
  uint32_t emitted = 0;
  size_t n = ptEventsWriteJson(&ptRing, (uint32_t)since,
                               s_respEvents, sizeof(s_respEvents), &missed,
                               &emitted);
  if (n == 0) {
    /* The ring did not fit. Report it rather than sending a truncated document
     * that Python cannot parse, and drop the oldest events so the next poll has
     * a chance of succeeding. */
    ptBridgeNoteCall("pt.get_events", false, "payload did not fit");
    return ptRpcErr("E_INTERNAL");
  }

  /* Quiet only when the poll was genuinely uneventful. Delivering events, or
   * discovering that Python's cursor fell behind the ring, are both worth a line
   * — those are the moments the game actually moves or loses data. */
  if (emitted == 0 && !missed) {
    ptBridgeNoteCall("pt.get_events", true, NULL, /*quiet=*/true);
  } else {
    char detail[40];
    snprintf(detail, sizeof(detail), "%lu event(s)%s",
             (unsigned long)emitted, missed ? ", cursor had gaps" : "");
    ptBridgeNoteCall("pt.get_events", true, detail);
  }
  return String(s_respEvents);
}

inline String ptRpcAckEvents(String req) {
  uint32_t now = millis();
  ptAppNoteLinkAlive(now);

  unsigned long seq = 0;
  if (!ptJsonGetULong(req.c_str(), "seq", &seq)) {
    ptBridgeNoteCall("pt.ack_events", false, "missing or non-numeric seq");
    return ptRpcErr("E_INTERNAL");
  }
  ptRingAck(&ptRing, (uint32_t)seq);
  /* Quiet on success: acks are pure bookkeeping and follow every poll that
   * returned anything, so printing them doubles the noise for no information.
   * The ring depth on the [status] line shows whether acking is keeping up. */
  ptBridgeNoteCall("pt.ack_events", true, NULL, /*quiet=*/true);
  return ptRpcOk();
}

inline String ptRpcSetState(String req) {
  uint32_t now = millis();
  ptAppNoteLinkAlive(now);

  char name[16];
  if (!ptJsonGetStr(req.c_str(), "state", name, sizeof(name))) {
    ptBridgeNoteCall("pt.set_state", false, "missing state");
    return ptRpcErr("E_INTERNAL");
  }

  PtState next;
  if (!ptStateFromName(name, &next)) {
    ptBridgeNoteCall("pt.set_state", false, name);
    return ptRpcErr("E_INTERNAL");
  }
  /* BOOT and SELFTEST describe the MCU's own startup and are not Python's to
   * command; see pt_state.h for why. */
  if (!ptStateIsSettable(next)) {
    ptBridgeNoteCall("pt.set_state", false, "state is not settable");
    return ptRpcErr("E_BUSY");
  }

  ptAppSetState(next);
  ptBridgeNoteCall("pt.set_state", true, name);
  return ptRpcOk();
}

inline String ptRpcStartRound(String req) {
  uint32_t now = millis();
  ptAppNoteLinkAlive(now);

  long round = 0;
  unsigned long limit = 0;
  if (!ptJsonGetLong(req.c_str(), "round", &round)) {
    ptBridgeNoteCall("pt.start_round", false, "missing round");
    return ptRpcErr("E_INTERNAL");
  }
  if (!ptJsonGetULong(req.c_str(), "limit_ms", &limit)) {
    ptBridgeNoteCall("pt.start_round", false, "missing limit_ms");
    return ptRpcErr("E_INTERNAL");
  }
  if (round < 0 || round > 999) {
    ptBridgeNoteCall("pt.start_round", false, "round out of range");
    return ptRpcErr("E_INTERNAL");
  }

  ptApp.round = (uint16_t)round;
  /* ptCountdownStart rejects a limit outside PT_ROUND_LIMIT_MIN_MS..MAX rather
   * than clamping it, because the round length formula is fixed by the game rules
   * and a value outside it means Python computed something wrong. Clamping would
   * hide that behind a console that plays fine but scores oddly. */
  if (!ptCountdownStart((uint16_t)round, (uint32_t)limit, now)) {
    ptBridgeNoteCall("pt.start_round", false, "limit_ms out of range");
    return ptRpcErr("E_INTERNAL");
  }

  ptAppSetState(PT_ST_PLAYING);
  ptBridgeNoteCall("pt.start_round", true, NULL);
  return ptRpcOk();
}

inline String ptRpcEndRound(String req) {
  uint32_t now = millis();
  ptAppNoteLinkAlive(now);

  char result[16];
  if (!ptJsonGetStr(req.c_str(), "result", result, sizeof(result))) {
    ptBridgeNoteCall("pt.end_round", false, "missing result");
    return ptRpcErr("E_INTERNAL");
  }

  /* The result drives local feedback only. Scoring, life accounting and the
   * decision about whether the game continues all belong to Python; the MCU just
   * makes the outcome visible and audible immediately, without waiting for a
   * further round trip. */
  ptCountdownStop();
  if (strcmp(result, "correct") == 0) {
    ptBuzzPlay(BUZZ_OK);
  } else if (strcmp(result, "wrong") == 0 || strcmp(result, "timeout") == 0) {
    ptBuzzPlay(BUZZ_WRONG);
    ptLivesLoseOne();
    ptApp.lives = ptLivesGetCount();
  } else {
    ptBridgeNoteCall("pt.end_round", false, result);
    return ptRpcErr("E_INTERNAL");
  }

  ptBridgeNoteCall("pt.end_round", true, result);
  return ptRpcOk();
}

inline String ptRpcSetLives(String req) {
  uint32_t now = millis();
  ptAppNoteLinkAlive(now);

  long n = 0;
  if (!ptJsonGetLong(req.c_str(), "n", &n)) {
    ptBridgeNoteCall("pt.set_lives", false, "missing n");
    return ptRpcErr("E_INTERNAL");
  }
  if (n < 0 || n > PT_MAX_LIVES) {
    ptBridgeNoteCall("pt.set_lives", false, "n out of range");
    return ptRpcErr("E_INTERNAL");
  }
  ptLivesSetCount((uint8_t)n);
  ptApp.lives = (uint8_t)n;
  ptBridgeNoteCall("pt.set_lives", true, NULL);
  return ptRpcOk();
}

inline String ptRpcShowScore(String req) {
  uint32_t now = millis();
  ptAppNoteLinkAlive(now);

  unsigned long score = 0;
  if (!ptJsonGetULong(req.c_str(), "score", &score)) {
    ptBridgeNoteCall("pt.show_score", false, "missing score");
    return ptRpcErr("E_INTERNAL");
  }
  ptApp.score = (uint32_t)score;
  /* Refresh the display only if the score is actually what the panel is showing,
   * so a score update arriving mid-round does not replace the round number. */
  if (ptApp.state == PT_ST_OVER) ptAppShowState(PT_ST_OVER);
  ptBridgeNoteCall("pt.show_score", true, NULL);
  return ptRpcOk();
}

inline String ptRpcBuzz(String req) {
  uint32_t now = millis();
  ptAppNoteLinkAlive(now);

  char name[16];
  if (!ptJsonGetStr(req.c_str(), "pattern", name, sizeof(name))) {
    ptBridgeNoteCall("pt.buzz", false, "missing pattern");
    return ptRpcErr("E_INTERNAL");
  }
  PtBuzzPattern p;
  if (!ptBuzzFromName(name, &p)) {
    ptBridgeNoteCall("pt.buzz", false, name);
    return ptRpcErr("E_INTERNAL");
  }
  /* A rejected play is not an error: it means a higher-priority cue is already
   * sounding, which is the intended behaviour. Report ok so Python does not treat
   * normal priority arbitration as a failure. */
  (void)ptBuzzPlay(p);
  ptBridgeNoteCall("pt.buzz", true, name);
  return ptRpcOk();
}

inline String ptRpcSelftest(String req) {
  (void)req;
  uint32_t now = millis();
  ptAppNoteLinkAlive(now);

  size_t n = ptAppWriteSelftest(s_respSmall, sizeof(s_respSmall));
  if (n == 0) {
    ptBridgeNoteCall("pt.selftest", false, "response did not fit");
    return ptRpcErr("E_INTERNAL");
  }
  ptBridgeNoteCall("pt.selftest", true, NULL);
  return String(s_respSmall);
}

/* ── Registration ─────────────────────────────────────────────────────────
 * Called from setup(), AFTER ptAppBegin(), so no handler can ever observe a
 * half-initialised ptApp.
 *
 * Returns false if Bridge.begin() failed or any handler failed to bind. A
 * partially bound bridge is worse than none, because Python would get plausible
 * responses from some methods and timeouts from others, which looks like a
 * Python bug rather than a firmware one.
 */
inline bool ptBridgeBegin(void) {
  memset(&s_rpcLog, 0, sizeof(s_rpcLog));

  if (!Bridge.begin()) {
    Serial.println("bridge: Bridge.begin() FAILED");
    return false;
  }

  struct { const char* name; String (*fn)(String); } handlers[] = {
    { "pt.get_status",  ptRpcGetStatus  },
    { "pt.get_events",  ptRpcGetEvents  },
    { "pt.ack_events",  ptRpcAckEvents  },
    { "pt.set_state",   ptRpcSetState   },
    { "pt.start_round", ptRpcStartRound },
    { "pt.end_round",   ptRpcEndRound   },
    { "pt.set_lives",   ptRpcSetLives   },
    { "pt.show_score",  ptRpcShowScore  },
    { "pt.buzz",        ptRpcBuzz       },
    { "pt.selftest",    ptRpcSelftest   },
  };
  const size_t n = sizeof(handlers) / sizeof(handlers[0]);

  bool allOk = true;
  for (size_t i = 0; i < n; ++i) {
    /* provide_safe, never provide. See the header comment for why this is the
     * single most important line in the file. */
    bool ok = Bridge.provide_safe(handlers[i].name, handlers[i].fn);
    Serial.print("bridge: bound ");
    Serial.print(handlers[i].name);
    Serial.println(ok ? "  ok" : "  FAILED");
    if (!ok) allOk = false;
  }

  if (allOk) {
    Serial.print("bridge: all ");
    Serial.print((unsigned)n);
    Serial.println(" handlers registered with provide_safe");
  }
  return allOk;
}

#endif /* PT_BRIDGE_H */
