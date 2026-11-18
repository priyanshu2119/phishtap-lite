/*
 * PhishTap Lite — production firmware (Stage 1b)
 *
 * The application orchestrator: shared state, the boot sequence, and the loop
 * that advances every engine.
 *
 * WHERE ALL THE SHARED STATE LIVES, AND WHY THAT IS SAFE
 * ptApp is a single plain struct, written by the bridge handlers and read by the
 * loop. There is no mutex and no volatile anywhere in this firmware, and that is
 * a consequence of a verified fact rather than an oversight.
 *
 * The core's main is:
 *
 *     for (;;) { loop(); __loopHook(); }
 *
 * (cores/arduino/main.cpp, arduino:zephyr 1.1.0). __loopHook is a weak symbol
 * that Arduino_RouterBridge overrides to call safeUpdate(), which dispatches
 * every handler registered with provide_safe. So safe handlers run on the SAME
 * THREAD as loop(), strictly after it returns, never concurrently with it.
 *
 * Two consequences shape this whole file:
 *
 *   1. No synchronisation is needed. Producers and consumers cannot interleave,
 *      so adding a lock would add only a way to deadlock.
 *   2. A handler that blocks stalls loop(), which stalls the RFID poll, the
 *      countdown and every animation at once. Handlers therefore do the smallest
 *      possible thing: parse, mutate ptApp, return. All the consequences of a
 *      state change are worked out here in the loop, on the next iteration.
 *
 * Handlers registered with plain provide() would run on the background RPC
 * thread instead, where Arduino APIs are unsafe and concurrency is real. This
 * firmware uses provide_safe exclusively, per docs/ARCHITECTURE.md §4.
 *
 * THE MCU DOES NOT RUN THE GAME
 * It reports what the hardware did and renders what Python tells it to. A card
 * tap becomes an event and a state change to TAPPED; whether that tap creates a
 * session, is refused as busy, or is rejected as a duplicate is Python's
 * decision, delivered back through pt.set_state. Keeping that authority on one
 * side is what stops the two processors from disagreeing about the game.
 */

#ifndef PT_APP_H
#define PT_APP_H

#include <Arduino.h>

#include "pt_config.h"
#include "pt_pins.h"
#include "pt_json.h"
#include "pt_state.h"
#include "pt_events.h"
#include "pt_font.h"
#include "pt_matrix.h"
#include "pt_buzzer.h"
#include "pt_lives.h"
#include "pt_rgb.h"
#include "pt_countdown.h"
#include "pt_rfid.h"

/* Two-level stringification, so PT_PROTO_VERSION expands to its value before
 * being turned into a literal. Without the extra level the banner would print
 * the token name "PT_PROTO_VERSION" instead of "1". */
#define PT_STRINGIFY_INNER(x) #x
#define PT_STRINGIFY(x) PT_STRINGIFY_INNER(x)

/* ── Shared state ───────────────────────────────────────────────────────── */

typedef struct {
  /* Identity and health, reported by pt.get_status. */
  PtState  state;
  bool     rfidOk;
  /* matrixInit records that ptMatrixBegin() ran. It is NOT a pass/fail test and
   * can never be one: the library's begin() returns 1 unconditionally and an LED
   * matrix has no readback, so software cannot tell whether any light came out.
   * It is serialised as `matrix_ok` for protocol compatibility with
   * docs/ARCHITECTURE.md §4, and pt.selftest reports matrix_has_readback=false so
   * nothing downstream mistakes it for a real verification. */
  bool     matrixInit;
  uint8_t  rfidVersion;
  bool     rfidVersionDocumented;

  /* Game state, owned by Python and mirrored here for display. */
  uint16_t round;
  uint32_t score;
  uint8_t  lives;

  /* Liveness. lastRpcMs is refreshed by EVERY handler, not just heartbeats, so
   * any traffic at all counts as proof the link is up. Relying only on an
   * explicit heartbeat would declare the link dead whenever Python was simply
   * busy doing something more useful than saying hello. */
  uint32_t lastRpcMs;
  bool     linkOk;
  bool     linkLostAnnounced;   /* the loss has been noticed; do not re-announce */
  bool     linkBlinking;        /* the panel is currently showing the fault blink */
  uint32_t linkLostSinceMs;     /* when the blink started, so it can be ended */

  /* Boot selftest. */
  bool     selftestDone;
  bool     selftestLeds;
  bool     selftestBuzzer;
  bool     selftestMatrix;
  bool     selftestRgb;
  bool     selftestRfid;
  uint8_t  bootStep;
  uint32_t bootStepMs;

  /* Scheduling. */
  uint32_t lastHeartbeatMs;
  uint32_t lastDisplayMs;
  uint32_t lastRecoverMs;
  uint32_t lastStatusLogMs;

  /* Total RPC calls served since boot, incremented by ptBridgeNoteCall.
   *
   * It lives here rather than in pt_bridge.h because the [status] line prints it,
   * and pt_app.h is included BEFORE pt_bridge.h, so the bridge cannot expose a
   * getter that this file could call. The bridge writes, the app displays.
   *
   * It exists because routine polls are no longer printed individually. Without a
   * cumulative count there would be no way to tell "Python is polling normally"
   * from "Python stopped polling five minutes ago", and the second one is exactly
   * the failure the link supervision is supposed to catch. */
  uint32_t rpcCalls;
} PtAppState;

static PtAppState   ptApp;
static PtEventRing  ptRing;

/* ── Display ──────────────────────────────────────────────────────────────
 * One place decides what the matrix shows for a given state, so the mapping
 * cannot drift between the state-change path and the periodic refresh path.
 *
 * Scrolling rather than static text wherever the message is longer than two
 * characters: the panel is 13 columns and a glyph is 5, so two characters is the
 * most that fits legibly at once.
 */
inline void ptAppShowState(PtState s) {
  switch (s) {
    case PT_ST_BOOT:
      ptMatrixAnimStart(PT_MXA_PULSE);
      break;
    case PT_ST_SELFTEST:
      /* The boot sequence drives the matrix itself, step by step. */
      break;
    case PT_ST_IDLE:
      /* Attract mode. Loops forever, because a console sitting idle with a blank
       * screen looks broken even when it is not. */
      ptMatrixScrollStart("PHISHTAP TAP CARD", true);
      break;
    case PT_ST_TAPPED:
      ptMatrixScrollStart("SCAN QR CODE", true);
      break;
    case PT_ST_PLAYING: {
      /* Show the round number. Two characters covers rounds 1-99, which is far
       * beyond the 40-item corpus, so no scrolling is needed here and the number
       * stays readable at a glance during play. */
      char label[3];
      uint16_t r = ptApp.round;
      if (r > 99) r = 99;
      if (r >= 10) {
        label[0] = (char)('0' + (r / 10));
        label[1] = (char)('0' + (r % 10));
        label[2] = '\0';
      } else {
        label[0] = (char)('0' + r);
        label[1] = '\0';
      }
      ptMatrixShowText2(label);
      break;
    }
    case PT_ST_OVER: {
      /* The score can exceed two digits, so it scrolls once and then holds. */
      char buf[12];
      snprintf(buf, sizeof(buf), "%lu", (unsigned long)ptApp.score);
      ptMatrixScrollStart(buf, true);
      break;
    }
    case PT_ST_BUSY:
      ptMatrixShowText2("BU");
      break;
    case PT_ST_ERROR:
      ptMatrixShowText2("ER");
      break;
    default:
      ptMatrixOff();
      break;
  }
}

/* Change state, and apply everything that follows from the change.
 *
 * This is the ONLY function that writes ptApp.state. Funelling every transition
 * through one place is what makes the display, the RGB LED and the countdown
 * impossible to leave inconsistent with the state, which is the bug class that
 * appears whenever several call sites each remember to update two of the three.
 */
inline void ptAppSetState(PtState next) {
  if (next == ptApp.state) return;
  PtState prev = ptApp.state;
  ptApp.state = next;

  ptRgbSetState(next);

  /* Leaving PLAYING must stop the countdown, or a round timer would keep ticking
   * and eventually fire a timeout for a round that is already over. */
  if (prev == PT_ST_PLAYING && next != PT_ST_PLAYING) {
    ptCountdownStop();
  }

  ptAppShowState(next);
}

/* ── Link loss ────────────────────────────────────────────────────────────
 * If Python stops talking for PT_LINK_LOSS_MS the console cannot trust anything
 * it is being told, so it returns to a safe, self-explanatory state: idle, with
 * the panel blinking so anyone nearby can see something is wrong.
 *
 * The blink is announced once rather than every iteration, because re-arming the
 * animation on every loop pass would restart it constantly and it would never
 * actually blink. It is also BOUNDED: after PT_LINK_LOST_BLINK_MS the console
 * stops blinking and returns to attract mode, so it stays usable and testable
 * while the link is down. linkOk remains false throughout.
 *
 * Two separate flags because they mean two different things. linkLostAnnounced
 * suppresses re-detecting the same outage; linkBlinking tracks whether the panel
 * is currently showing the fault animation. Clearing the first to stop the blink
 * would make the outage re-announce itself on the very next iteration.
 */
inline void ptAppEnterLinkLost(uint32_t now) {
  if (ptApp.linkLostAnnounced) return;
  ptApp.linkLostAnnounced = true;
  ptApp.linkBlinking      = true;
  ptApp.linkLostSinceMs   = now;
  ptApp.linkOk            = false;

  ptCountdownStop();
  ptAppSetState(PT_ST_IDLE);
  /* Override the idle attract scroll with the fault animation. SetState has
   * already run, so this is a deliberate correction rather than a race. */
  ptMatrixAnimStart(PT_MXA_BLINK);
  ptRgbAllOff();
  ptBuzzPlay(BUZZ_ERROR);
  ptRingPushHwFault(&ptRing, now, PT_FAULT_BRIDGE);

  /* Say so on the console. Without this the panel blinking is the only clue, and
   * "the whole screen is flashing blue" reads as a hardware fault rather than as
   * the safety mechanism working exactly as designed. */
  Serial.println("[link] LOST - no RPC from Python for 15s.");
  Serial.println("[link]   panel will blink for 6s then return to attract mode.");
  Serial.println("[link]   this is expected while no Python side is polling.");
}

/* Stop the fault blink and restore the normal display, without pretending the
 * link came back. Called once the announcement period has elapsed. */
inline void ptAppEndLinkBlink(void) {
  if (!ptApp.linkBlinking) return;
  ptApp.linkBlinking = false;
  ptRgbApplyState(ptApp.state);
  ptAppShowState(ptApp.state);
  Serial.println("[link] still down, returning to attract mode so the console "
                 "stays usable");
}

inline void ptAppNoteLinkAlive(uint32_t now) {
  ptApp.lastRpcMs = now;
  if (!ptApp.linkOk) {
    /* The link came back. Restore the normal display for the current state. */
    bool wasLost = ptApp.linkLostAnnounced;
    ptApp.linkOk            = true;
    ptApp.linkLostAnnounced = false;
    ptApp.linkBlinking      = false;
    ptRgbApplyState(ptApp.state);
    ptAppShowState(ptApp.state);
    if (wasLost) Serial.println("[link] RESTORED - Python is talking again");
  }
}

/* ── Boot selftest ────────────────────────────────────────────────────────
 * Non-blocking, and deliberately so. A blocking selftest in setup() would delay
 * handler registration, and Python calling pt.get_status during that window
 * would get nothing back and conclude the firmware was dead. Running it as a
 * state means the bridge is live from the first millisecond and Python can watch
 * the console come up.
 *
 * Each step lasts PT_BOOT_SELFTEST_MS / 4 and confirms one subsystem visibly.
 * The results are latched into ptApp so pt.selftest can report them afterwards
 * without re-running anything.
 */
#define PT_BOOT_STEP_MS (PT_BOOT_SELFTEST_MS / 4)

inline void ptAppBootStep(uint32_t now) {
  if ((uint32_t)(now - ptApp.bootStepMs) < PT_BOOT_STEP_MS) return;
  ptApp.bootStepMs = now;
  ptApp.bootStep++;

  switch (ptApp.bootStep) {
    case 1:
      /* Everything on at once: proves every LED, every matrix pixel and the
       * buzzer are reachable in a single unmistakable moment. */
      ptMatrixAllOn();
      ptLivesSetCount(PT_MAX_LIVES);
      ptRgbSet(PT_RGB_LED3, true, true, true);
      ptBuzzPlay(BUZZ_WELCOME);
      ptApp.selftestLeds   = true;
      ptApp.selftestRgb    = true;
      ptApp.selftestBuzzer = true;
      break;
    case 2:
      ptMatrixShowText2("88");
      ptLivesSetCount(2);
      break;
    case 3:
      ptMatrixShowText2("OK");
      ptLivesSetCount(1);
      break;
    case 4:
      /* Finished. Hand over to IDLE, which sets the attract animation and the
       * state colour. Lives go to the game's starting count. */
      ptApp.selftestDone   = true;
      ptApp.selftestMatrix = ptApp.matrixInit;
      ptApp.selftestRfid   = ptApp.rfidOk;
      ptLivesSetCount(PT_MAX_LIVES);
      /* Only advance to IDLE if boot did not already put us in ERROR.
       *
       * This used to set IDLE unconditionally, which silently erased a real
       * hardware failure: ptAppBegin() sets ERROR when the reader or matrix
       * fails, then this step ran a second later and overwrote it, so the
       * console looked healthy while its status payload said otherwise. A
       * selftest must not be able to clear the verdict it was asked to report. */
      if (ptApp.state != PT_ST_ERROR) {
        ptAppSetState(PT_ST_IDLE);
        /* Announce the handover. Without this the last thing on the console is
         * the boot banner, and a selftest that quietly stopped advancing would
         * look identical to one that completed. */
        Serial.println("[boot] selftest complete, entering IDLE / attract mode");
      } else {
        Serial.println("[boot] selftest finished, staying in ERROR - a subsystem "
                       "failed to initialise");
      }
      break;
    default:
      break;
  }
}

/* ── Setup ────────────────────────────────────────────────────────────────
 * Called from setup(). Order matters:
 *   1. Serial/Monitor first, so any failure below can be reported.
 *   2. Outputs before the reader, so a boot beep or stray LED cannot happen
 *      while the reader is being reset.
 *   3. The reader last, because its hardware reset blocks for 70 ms and nothing
 *      else needs to run during that.
 * The bridge is registered by the caller AFTER this returns, so that no handler
 * can observe a half-initialised ptApp.
 */
inline void ptAppBegin(void) {
  memset(&ptApp, 0, sizeof(ptApp));
  ptRingInit(&ptRing);

  /* Serial and Monitor are the SAME object on this core: singletons.cpp declares
   * `extern BridgeMonitor<> Serial [[gnu::alias("Monitor")]];`. The baud argument
   * is ignored by BridgeMonitor::begin, which does `(void)_legacy_baud;`, so the
   * rate set here is cosmetic and the App Lab console speed does not matter. */
  Serial.begin(115200);

  uint32_t now = millis();

  ptBuzzBegin();
  ptLivesBegin();
  ptRgbBegin();
  ptCountdownBegin();

  ptMatrixBegin();
  ptApp.matrixInit = true;   /* initialisation ran; see the field comment */
  ptApp.rfidOk     = ptRfidBegin();
  ptApp.rfidVersion           = ptRfidGetVersion();
  ptApp.rfidVersionDocumented = ptRfidVersionIsDocumented();

  ptApp.state            = PT_ST_BOOT;
  ptApp.lives            = PT_MAX_LIVES;
  ptApp.round            = 0;
  ptApp.score            = 0;
  ptApp.linkOk           = false;   /* nothing heard from Python yet */
  ptApp.lastRpcMs        = now;
  ptApp.lastHeartbeatMs  = now;
  ptApp.lastDisplayMs    = now;
  ptApp.lastRecoverMs    = now;
  ptApp.lastStatusLogMs  = now;
  ptApp.bootStep         = 0;
  ptApp.bootStepMs       = now;

  ptRgbSetState(PT_ST_BOOT);

  Serial.println();
  Serial.println("PhishTap Lite firmware " PT_FW_VERSION
                 " (protocol v" PT_STRINGIFY(PT_PROTO_VERSION) ")");
  Serial.print("  matrix: ");
  Serial.println(ptApp.matrixInit ? "initialised (no readback, confirm by eye)"
                                  : "FAILED");
  Serial.print("  rfid:   ");
  if (ptApp.rfidOk) {
    Serial.print("ok, version 0x");
    Serial.println(ptApp.rfidVersion, HEX);
    if (!ptApp.rfidVersionDocumented) {
      /* Say so plainly. An undocumented version is a counterfeit die, which is
       * normal and works; staying quiet about it is what made the bring-up
       * failures look mysterious. */
      Serial.println("          (undocumented version: counterfeit die, "
                     "expected and not a fault)");
    }
  } else {
    Serial.print("FAILED, version 0x");
    Serial.println(ptApp.rfidVersion, HEX);
  }

  if (!ptApp.rfidOk) {
    ptRingPushHwFault(&ptRing, now, PT_FAULT_RFID_INIT);
    ptAppSetState(PT_ST_ERROR);
  } else {
    ptAppSetState(PT_ST_SELFTEST);
  }
}

/* One-line status summary, printed every PT_STATUS_LOG_MS.
 *
 * This exists because the firmware would otherwise be silent. It prints at boot
 * and in response to an RPC, and with no Python side polling there are no RPCs,
 * so the console goes quiet after the first second. Silence is indistinguishable
 * from a crash, which makes bring-up impossible to observe and invites someone to
 * reset a board that is working perfectly.
 *
 * Kept to a single line so it can be watched scroll without flooding, and it
 * carries the counters that actually diagnose something: the state, whether the
 * reader is alive and in which sub-state, whether the link is up, and how full
 * the ring is. */
inline void ptAppLogStatus(uint32_t now) {
  Serial.print("[status] state=");
  Serial.print(ptStateName(ptApp.state));
  Serial.print(" rfid=");
  Serial.print(ptApp.rfidOk ? "ok" : "FAULT");
  Serial.print("(v0x");
  if (ptApp.rfidVersion < 0x10) Serial.print('0');
  Serial.print(ptApp.rfidVersion, HEX);
  Serial.print(") rfid_st=");
  Serial.print(ptRfidStateName(ptRfidGetState()));
  Serial.print(" link=");
  Serial.print(ptApp.linkOk ? "up" : "DOWN");
  Serial.print(" ring=");
  Serial.print((unsigned)ptRingLen(&ptRing));
  Serial.print("/");
  Serial.print((unsigned)PT_EVENT_RING_SIZE);
  Serial.print(" ovf=");
  Serial.print((unsigned long)ptRing.overflows);
  Serial.print(" rpc=");
  Serial.print((unsigned long)ptApp.rpcCalls);
  Serial.print(" up=");
  Serial.print((unsigned long)(now / 1000));
  Serial.println("s");
}

/* ── Loop ─────────────────────────────────────────────────────────────────
 * Called from loop(). Every step is non-blocking; the whole function returns in
 * well under a millisecond when nothing is due, which is what keeps the bridge
 * handlers responsive.
 *
 * `now` is read ONCE and passed to every engine. Letting each engine call
 * millis() for itself allows them to disagree about the current time by a
 * millisecond, which is harmless most of the time and very confusing when
 * debugging something that is not.
 */
inline void ptAppLoop(void) {
  uint32_t now = millis();

  /* 1. Link supervision. Checked first so a dead link overrides everything else
   *    rather than being noticed after a round of normal work. */
  if ((uint32_t)(now - ptApp.lastRpcMs) >= PT_LINK_LOSS_MS) {
    ptAppEnterLinkLost(now);
  }
  /* End the fault blink after its bounded announcement, so the console returns to
   * attract mode and stays testable while the link is still down. */
  if (ptApp.linkBlinking &&
      (uint32_t)(now - ptApp.linkLostSinceMs) >= PT_LINK_LOST_BLINK_MS) {
    ptAppEndLinkBlink();
  }

  /* 2. Boot sequence, before any game logic. Nothing else should act until the
   *    selftest has finished and the hardware is known good. */
  if (!ptApp.selftestDone) {
    ptAppBootStep(now);
    ptMatrixTick(now);
    ptBuzzTick();
    ptRgbTick();
    return;
  }

  /* 3. Round countdown. Ticks drive the buzzer; a timeout becomes an event for
   *    Python rather than a local decision, because the server owns timing. */
  uint8_t cd = ptCountdownTick(now);
  if (cd & PT_CD_TICK) {
    ptBuzzPlay(BUZZ_TICK);
  }
  if (cd & PT_CD_TIMEOUT) {
    ptRingPushRoundTimeout(&ptRing, now, ptCountdownRound());
  }

  /* 4. Reader. */
  uint8_t ev = ptRfidTick(now);

  if (ev & PT_RFID_EV_FAULT) {
    ptApp.rfidOk = false;
    ptRingPushHwFault(&ptRing, now, PT_FAULT_RFID_READ);
    ptAppSetState(PT_ST_ERROR);
    ptBuzzPlay(BUZZ_ERROR);
  }

  if (ev & PT_RFID_EV_REMOVED) {
    ptRingPushCardRemoved(&ptRing, now);
    Serial.println("[rfid] card removed");
  }

  if (ev & PT_RFID_EV_TAP) {
    /* Queue the tap first, so the event exists even if the state change below is
     * refused. Losing a tap silently is the worst failure this firmware can
     * have, because from the outside it is indistinguishable from a dead reader. */
    ptRingPushCardTap(&ptRing, now, ptRfidGetUidHex(), ptRfidGetUidLen());

    /* Decide the outcome ONCE, then log it and act on it.
     *
     * Evaluating the condition separately for the log line and for the action is
     * how the two drift apart, and that is precisely what happened when the
     * link-down case was added: the action gained a third branch while the log
     * still reported a two-way "accepted / REFUSED (busy)", so a tap that stayed
     * in IDLE was logged as accepted. A single decision variable makes that
     * impossible. It is also evaluated before any state change, so the answer
     * cannot be affected by the action it selects. */
    uint8_t action;
    if (!ptApp.linkOk)                          action = 0;   /* nobody to hand over to */
    else if (ptStateAcceptsTap(ptApp.state))    action = 1;   /* accept */
    else                                        action = 2;   /* busy */

    /* Log it. This is the single most useful line in the whole firmware during
     * bring-up: it proves the reader, the state machine and the ring all work,
     * with no Python side required to observe any of it. */
    Serial.print("[rfid] CARD TAP uid=");
    Serial.print(ptRfidGetUidHex());
    Serial.print(" len=");
    Serial.print((unsigned)ptRfidGetUidLen());
    Serial.print(" sak=0x");
    Serial.print(ptRfidGetSak(), HEX);
    Serial.print(" seq=");
    Serial.print((unsigned long)ptRing.newestSeq);
    Serial.print(" state=");
    Serial.print(ptStateName(ptApp.state));
    switch (action) {
      case 0:  Serial.println("  queued, NO LINK so staying idle"); break;
      case 1:  Serial.println("  ACCEPTED -> tapped");              break;
      default: Serial.println("  REFUSED (busy)");                  break;
    }

    if (action == 1) {
      ptAppSetState(PT_ST_TAPPED);
      ptBuzzPlay(BUZZ_WELCOME);
    } else if (action == 0) {
      /* The link is down, so nobody will ever answer this tap.
       *
       * Latching into TAPPED here would be actively harmful: TAPPED is only
       * released by Python calling pt.set_state, so with no Python side the
       * console would sit in TAPPED forever and refuse every subsequent tap as
       * busy. That is exactly what the first hardware run showed - one accepted
       * tap, then REFUSED (busy) for the rest of the session, with "BU" stuck on
       * the panel.
       *
       * The event is still queued above, so nothing is lost if Python comes back.
       * We just do not hand over to a party that is not there, and the console
       * stays in IDLE so the reader remains testable. */
      ptMatrixShowText2("NL");      /* no link */
      ptBuzzPlay(BUZZ_JOIN);
    } else {
      /* A tap while busy is refused locally for immediate feedback, but the
       * event is still queued so Python can log it and decide what to do. */
      ptMatrixShowText2("BU");
      ptBuzzPlay(BUZZ_BUSY);
    }
  }

  /* 5. Periodic recovery attempt if the reader faulted. Rate-limited, because a
   *    reader that has genuinely failed must not be re-initialised on every loop
   *    iteration: that would starve the other engines and spend all its time
   *    resetting rather than trying to read. */
  if (!ptApp.rfidOk && (uint32_t)(now - ptApp.lastRecoverMs) >= PT_RFID_RECOVER_MS) {
    ptApp.lastRecoverMs = now;
    if (ptRfidTryRecover()) {
      ptApp.rfidOk = true;
      /* REFRESH THE CACHED VERSION. This was missing, and it produced the most
       * confusing line in the first hardware run:
       *
       *     [status] state=idle rfid=ok(v0x00) rfid_st=seek ...
       *
       * rfid=ok next to v0x00 is self-contradictory, because 0x00 is the one
       * value that means "nothing answered". What had happened is that the first
       * init at cold boot read 0x00 and faulted, the recovery path then
       * re-initialised successfully and set rfidOk, but the version field still
       * held the stale 0x00 from the failed attempt. The status line was
       * reporting a value that had been untrue for minutes.
       *
       * Any cached copy of hardware state must be refreshed whenever the
       * hardware is re-read, or the cache quietly becomes a lie. */
      ptApp.rfidVersion           = ptRfidGetVersion();
      ptApp.rfidVersionDocumented = ptRfidVersionIsDocumented();
      ptApp.selftestRfid          = true;

      Serial.print("[rfid] RECOVERED, version 0x");
      Serial.print(ptApp.rfidVersion, HEX);
      Serial.println(ptApp.rfidVersionDocumented ? "" : " (counterfeit die, expected)");

      ptRingPushHwFault(&ptRing, now, PT_FAULT_NONE);   /* recovery notice */
      if (ptApp.state == PT_ST_ERROR) ptAppSetState(PT_ST_IDLE);
    }
  }

  /* 6. Heartbeat. */
  if ((uint32_t)(now - ptApp.lastHeartbeatMs) >= PT_HEARTBEAT_MS) {
    ptApp.lastHeartbeatMs = now;
    ptRingPushHeartbeat(&ptRing, now);
  }

  /* 7. Periodic status line, so the console is never silent for long. */
  if ((uint32_t)(now - ptApp.lastStatusLogMs) >= PT_STATUS_LOG_MS) {
    ptApp.lastStatusLogMs = now;
    ptAppLogStatus(now);
  }

  /* 8. Output engines, last, so they render the state this iteration settled on
   *    rather than the one it started with. */
  ptMatrixTick(now);
  ptBuzzTick();
  ptLivesTick();
  ptRgbTick();
}

/* ── Status serialisation ─────────────────────────────────────────────────
 * Builds the pt.get_status response. Returns 0 if it did not fit, which the
 * caller must treat as an error rather than sending a truncated document.
 */
inline size_t ptAppWriteStatus(char* buf, size_t cap) {
  PtJsonWriter w;
  ptJsonInit(&w, buf, cap);

  ptJsonObjBegin(&w);
  ptJsonKVInt(&w, "proto", PT_PROTO_VERSION);
  ptJsonKVStr(&w, "fw", PT_FW_VERSION);
  ptJsonKVStr(&w, "state", ptStateName(ptApp.state));
  ptJsonKVUint(&w, "uptime_ms", (unsigned long)millis());
  ptJsonKVBool(&w, "rfid_ok", ptApp.rfidOk);
  ptJsonKVBool(&w, "matrix_ok", ptApp.matrixInit);
  ptJsonKVInt(&w, "lives", (long)ptApp.lives);
  ptJsonKVInt(&w, "round", (long)ptApp.round);
  ptJsonKVUint(&w, "score", (unsigned long)ptApp.score);
  ptJsonKVInt(&w, "queue_len", (long)ptRingLen(&ptRing));
  ptJsonKVUint(&w, "overflows", (unsigned long)ptRing.overflows);
  ptJsonKVBool(&w, "link_ok", ptApp.linkOk);
  /* Extra diagnostics beyond the architecture's list. Cheap to include, and
   * they are exactly what is needed to tell a counterfeit reader from a broken
   * one without reflashing anything. */
  ptJsonKVInt(&w, "rfid_version", (long)ptApp.rfidVersion);
  ptJsonKVBool(&w, "rfid_version_documented", ptApp.rfidVersionDocumented);
  ptJsonKVStr(&w, "rfid_state", ptRfidStateName(ptRfidGetState()));
  ptJsonObjEnd(&w);

  return ptJsonFinish(&w);
}

/* Builds the pt.selftest response. Reports what the boot sequence observed plus
 * the current health of each subsystem; it does not re-run anything, because a
 * selftest that flashes every LED on demand would be disruptive mid-game. */
inline size_t ptAppWriteSelftest(char* buf, size_t cap) {
  PtJsonWriter w;
  ptJsonInit(&w, buf, cap);

  ptJsonObjBegin(&w);
  ptJsonKVBool(&w, "leds", ptApp.selftestLeds);
  ptJsonKVBool(&w, "buzzer", ptApp.selftestBuzzer);
  ptJsonKVBool(&w, "matrix", ptApp.selftestMatrix && ptApp.matrixInit);
  ptJsonKVBool(&w, "rgb", ptApp.selftestRgb);
  ptJsonKVBool(&w, "rfid", ptApp.selftestRfid && ptApp.rfidOk);
  ptJsonKey(&w, "details");
  ptJsonObjBegin(&w);
  /* Stated explicitly so nobody reads matrix:true as "the panel was tested".
   * The panel cannot be tested by software; it has no readback. */
  ptJsonKVBool(&w, "matrix_has_readback", false);
  ptJsonKVBool(&w, "boot_selftest_done", ptApp.selftestDone);
  ptJsonKVInt(&w, "rfid_version", (long)ptApp.rfidVersion);
  ptJsonKVBool(&w, "rfid_version_documented", ptApp.rfidVersionDocumented);
  ptJsonKVStr(&w, "rfid_state", ptRfidStateName(ptRfidGetState()));
  ptJsonKVBool(&w, "link_ok", ptApp.linkOk);
  ptJsonKVUint(&w, "ring_overflows", (unsigned long)ptRing.overflows);
  ptJsonKVUint(&w, "ring_pushed", (unsigned long)ptRing.totalPushed);
  ptJsonObjEnd(&w);
  ptJsonObjEnd(&w);

  return ptJsonFinish(&w);
}

#endif /* PT_APP_H */
