/*
 * PhishTap Lite — phone game logic.
 *
 * THE SERVER IS THE AUTHORITY ON TIMING. This page renders a countdown from a
 * remaining-milliseconds figure the server supplies, and it never decides for
 * itself that a round ended. If the socket drops, if the tab is backgrounded, if
 * the phone's clock is wrong — the server still grades the timeout by its own
 * deadline. Everything here is presentation.
 *
 * That division is why a lost connection degrades rather than breaks: the player
 * keeps seeing rounds (via polling), and answers still arrive over REST, which is
 * the authoritative path. The socket only makes it feel instant.
 *
 * THE ANSWER IS NEVER ON THIS PAGE. The server sends items with
 * include_answer=False, so there is nothing in the DOM, the JS state or the
 * network traffic to find. Correctness feedback arrives only after the answer is
 * submitted, and the explanation only on the review screen.
 */

/* global PT */
(function () {
  "use strict";

  var POLL_MS = 1500;
  var VERDICT_MS = 1100;      /* how long the correct/wrong overlay stays up */

  var token = "";
  var socket = null;
  var socketLive = false;
  var countdown = null;
  var answered = false;       /* guards against a double tap scoring twice */
  var currentRound = null;    /* round number of the message on screen */
  var pollTimer = null;
  var reviewToken = null;

  var el = {};

  function cacheElements() {
    var ids = [
      "conn",
      "screen-loading", "loading-title", "loading-text",
      "screen-consent", "consent-title", "consent-body", "consent-pseudonymous",
      "consent-retention", "consent-norealdata", "nickname", "nickname-label",
      "nickname-hint", "consent-error", "btn-consent", "btn-decline",
      "consent-declined", "declined-text",
      "screen-play", "k-round", "v-round", "k-score", "v-score", "k-lives", "v-lives",
      "timer-fill", "timer-seconds", "timer-sr",
      "msg", "msg-sender", "msg-channel", "msg-subject", "msg-body", "msg-prompt",
      "btn-phish", "btn-phish-label", "btn-phish-hint",
      "btn-safe", "btn-safe-label", "btn-safe-hint",
      "verdict", "verdict-word", "verdict-sub", "verdict-pts",
      "screen-over", "over-eyebrow", "over-score", "over-rounds", "btn-review", "over-again",
      "screen-gone", "gone-title", "gone-text", "btn-gone-join", "gone-hint",
    ];
    for (var i = 0; i < ids.length; i++) { el[ids[i]] = PT.$(ids[i]); }
  }

  function applyStrings() {
    PT.setText(el["loading-title"], PT.t("brand.name"));
    PT.setText(el["loading-text"], PT.t("common.loading"));

    PT.setText(el["consent-title"], PT.t("game.consentTitle"));
    PT.setText(el["consent-body"], PT.t("game.consentBody"));
    PT.setText(el["consent-pseudonymous"], PT.t("game.consentPseudonymous"));
    PT.setText(el["consent-retention"], PT.t("game.consentRetention"));
    PT.setText(el["consent-norealdata"], PT.t("game.consentNoRealData"));
    PT.setText(el["nickname-label"], PT.t("game.nicknameLabel"));
    PT.setText(el["nickname-hint"], PT.t("game.nicknameHint"));
    PT.setText(el["btn-consent"], PT.t("game.consentAgree"));
    PT.setText(el["btn-decline"], PT.t("game.consentDecline"));
    PT.setText(el["declined-text"], PT.t("game.consentDeclined"));

    PT.setText(el["k-round"], PT.t("kiosk.roundLabel"));
    PT.setText(el["k-score"], PT.t("kiosk.scoreLabel"));
    PT.setText(el["k-lives"], PT.t("kiosk.livesLabel"));
    PT.setText(el["msg-prompt"], PT.t("game.questionPrompt"));
    PT.setText(el["btn-phish-label"], PT.t("game.buttonPhish"));
    PT.setText(el["btn-phish-hint"], PT.t("game.buttonPhishHint"));
    PT.setText(el["btn-safe-label"], PT.t("game.buttonSafe"));
    PT.setText(el["btn-safe-hint"], PT.t("game.buttonSafeHint"));

    PT.setText(el["over-eyebrow"], PT.t("game.gameOverTitle"));
    PT.setText(el["btn-review"], PT.t("game.seeReview"));
    PT.setText(el["over-again"], PT.t("game.playAgainHint"));

    PT.setText(el["gone-title"], PT.t("brand.name"));
    PT.setText(el["gone-text"], PT.t("game.sessionGone"));
    PT.setText(el["btn-gone-join"], PT.t("game.tryPinInstead"));
    PT.setText(el["gone-hint"], PT.t("game.goneHint"));
  }

  /* ── Screen switching ─────────────────────────────────────────────────── */

  var SCREENS = ["loading", "consent", "play", "over", "gone"];

  function showScreen(name) {
    for (var i = 0; i < SCREENS.length; i++) {
      PT.show(el["screen-" + SCREENS[i]], SCREENS[i] === name);
    }
  }

  function setConnBanner(text) {
    if (!text) { PT.show(el.conn, false); return; }
    PT.setText(el.conn, text);
    PT.show(el.conn, true);
  }

  function fatal(message) {
    stopPolling();
    if (countdown) { countdown.stop(); }
    PT.setText(el["gone-text"], message || PT.t("game.sessionGone"));
    showScreen("gone");
  }

  /* ── HUD ──────────────────────────────────────────────────────────────── */

  function hearts(n) {
    var count = Math.max(0, Math.min(9, Number(n) || 0));
    var out = "";
    for (var i = 0; i < count; i++) { out += "\u2665"; }
    return out || "\u2014";
  }

  function renderHud(payload) {
    if (!payload) { return; }
    if (payload.round !== undefined) {
      PT.setText(el["v-round"], String((Number(payload.round) || 0) + 1));
    }
    if (payload.score !== undefined) { PT.setText(el["v-score"], String(payload.score)); }
    if (payload.lives !== undefined) { PT.setText(el["v-lives"], hearts(payload.lives)); }
  }

  /* ── Rendering a round ────────────────────────────────────────────────── */

  function channelClass(channel) {
    if (channel === "whatsapp") { return "msg msg-whatsapp"; }
    if (channel === "sms") { return "msg msg-sms"; }
    return "msg";
  }

  function channelName(channel) {
    var map = {
      email: PT.t("game.channelEmail"),
      sms: PT.t("game.channelSms"),
      whatsapp: PT.t("game.channelWhatsapp"),
      poster: PT.t("game.channelPoster"),
    };
    return map[channel] || channel || "";
  }

  function renderRound(data) {
    var item = data && data.item;
    if (!item) { return; }

    currentRound = (data.round !== undefined) ? data.round : currentRound;
    answered = false;
    setAnswerButtonsEnabled(true);
    PT.show(el.verdict, false);

    el.msg.className = channelClass(item.channel);
    /* setText everywhere. The corpus is deliberately full of lookalike domains,
     * fake sender names and malicious-looking URLs; rendering any of it as HTML
     * would turn a teaching aid into an actual XSS vector, and rendering a link as
     * an anchor would make the one place students are told to be careful the one
     * place where tapping does something. */
    PT.setText(el["msg-sender"], item.sender || "");
    PT.setText(el["msg-channel"], channelName(item.channel));
    var hasSubject = !!item.subject;
    PT.show(el["msg-subject"], hasSubject);
    if (hasSubject) {
      PT.setText(el["msg-subject"], PT.t("game.subjectPrefix") + ": " + item.subject);
    }
    PT.setText(el["msg-body"], item.body || "");

    renderHud(data);

    /* Scroll back to the top: a long message from the previous round can leave the
     * body scrolled down, and the player would then miss the sender line, which is
     * often where the tell is. */
    var body = document.querySelector(".game-body");
    if (body) { body.scrollTop = 0; }

    startTimer(data);
    ackRendered();
  }

  function startTimer(data) {
    var limit = Number(data.limit_ms) || 0;
    var remaining = limit;

    /* Prefer the server's remaining figure if it supplies one; otherwise the limit
     * is the remaining time, which is correct at round start. */
    if (typeof data.remaining_ms === "number") { remaining = data.remaining_ms; }

    if (countdown) { countdown.stop(); }
    countdown = new PT.Countdown(onTimerTick, onTimerExpire);
    countdown.startWithTotal(remaining, limit || remaining || 1);
  }

  function onTimerTick(leftMs, fraction) {
    var pct = Math.max(0, Math.min(1, fraction)) * 100;
    el["timer-fill"].style.width = pct + "%";

    /* Colour shifts as time runs out. The bar alone is not enough for a
     * colour-blind player, so the numeric seconds are shown alongside it and the
     * two always agree. */
    el["timer-fill"].className = "timer-fill" +
      (fraction <= 0.25 ? " low" : (fraction <= 0.55 ? " mid" : ""));

    var secs = Math.ceil(leftMs / 1000);
    PT.setText(el["timer-seconds"], secs + PT.t("common.secondsShort"));

    /* Announced to screen readers only on whole seconds, not on every 50ms tick,
     * which would make it unusable. */
    var srText = PT.t("game.timeLeft", { seconds: secs });
    if (el["timer-sr"].textContent !== srText && leftMs % 1000 < 60) {
      PT.setText(el["timer-sr"], srText);
    }
  }

  /* The local timer hitting zero does NOT end the round. The server grades the
   * timeout by its own deadline and pushes round_result. This only stops the bar
   * and locks the buttons, because accepting an answer after the deadline would
   * be a second, contradictory source of truth. */
  function onTimerExpire() {
    setAnswerButtonsEnabled(false);
    el["timer-fill"].style.width = "0%";
    PT.setText(el["timer-seconds"], PT.t("game.timeUp"));
  }

  /* Tell the server the message is actually on screen. Reaction time is measured
   * from here rather than from round_start, because the gap between the two is
   * network and render latency that has nothing to do with how fast the student
   * read anything. Sent after a frame so the paint has happened. */
  function ackRendered() {
    var send = function () { PT.api.renderAck(token); };
    if (window.requestAnimationFrame) { window.requestAnimationFrame(send); }
    else { window.setTimeout(send, 0); }
  }

  function setAnswerButtonsEnabled(enabled) {
    el["btn-phish"].disabled = !enabled;
    el["btn-safe"].disabled = !enabled;
  }

  /* ── Answering ────────────────────────────────────────────────────────── */

  function submitAnswer(choice) {
    /* Locked the instant a button is pressed, before the request goes out. Two
     * fast taps would otherwise both be in flight and the second would arrive as
     * a duplicate the server rejects, which the player would experience as the
     * game ignoring them. */
    if (answered) { return; }
    answered = true;
    setAnswerButtonsEnabled(false);
    if (countdown) { countdown.stop(); }

    PT.api.answer(token, choice, "phone").then(function (res) {
      if (!res || !res.ok) {
        /* A rejected answer must not leave the player staring at locked buttons
         * with no explanation. Re-enable so they can retry, unless the session is
         * genuinely gone. */
        if (res && (res.err === "E_EXPIRED" || res.status === 404)) {
          fatal(res.message);
          return;
        }
        answered = false;
        setAnswerButtonsEnabled(true);
        setConnBanner((res && res.message) || PT.t("errors.generic"));
        window.setTimeout(function () { setConnBanner(null); }, 3000);
        return;
      }
      /* The REST response already carries the result, so show it immediately
       * rather than waiting for the socket push that says the same thing. */
      if (res.round !== undefined || res.correct !== undefined) { showVerdict(res); }
    });
  }

  function showVerdict(result) {
    var correct = !!result.correct;
    var timedOut = !!result.timed_out;

    el.verdict.className = "verdict " + (correct ? "good" : "bad");
    PT.setText(el["verdict-word"],
      correct ? PT.t("game.correct")
              : (timedOut ? PT.t("game.timeUp") : PT.t("game.wrong")));
    PT.setText(el["verdict-sub"],
      correct ? ""
              : (timedOut ? PT.t("game.timeoutLostLife") : PT.t("game.wrongLostLife")));
    PT.setText(el["verdict-pts"],
      correct && result.points ? PT.t("game.pointsEarned", { points: result.points }) : "");
    PT.show(el.verdict, true);

    /* Haptics plus a full-screen colour change. The vibration is behind feature
     * detection because iOS Safari does not implement it, and the colour is the
     * fallback that always works. */
    PT.vibrate(correct ? [40] : [90, 50, 90]);

    if (result.lives !== undefined) { PT.setText(el["v-lives"], hearts(result.lives)); }
    if (result.score !== undefined) { PT.setText(el["v-score"], String(result.score)); }
    if (result.streak !== undefined && result.streak > 1) {
      PT.setText(el["verdict-sub"], PT.t("game.streakBonus", { streak: result.streak }));
    }

    /* The overlay hides itself. The next round_start replaces the message beneath
     * it; if the game ended instead, game_over switches screens and the overlay is
     * hidden on the way out. */
    window.setTimeout(function () { PT.show(el.verdict, false); }, VERDICT_MS);
  }

  /* ── Game over ────────────────────────────────────────────────────────── */

  function showGameOver(data) {
    if (countdown) { countdown.stop(); }
    PT.show(el.verdict, false);
    stopPolling();

    PT.setText(el["over-score"], String((data && data.score) || 0));
    PT.setText(el["over-rounds"], PT.t("game.gameOverRounds", {
      count: (data && data.rounds_played) || 0,
    }));

    /* The review link needs the review token, which only game_over carries. If it
     * is missing the button is hidden rather than pointed at a URL that would
     * 404 — a dead button is worse than no button, because it looks broken. */
    var rt = (data && data.review_token) || reviewToken;
    if (rt) {
      reviewToken = rt;
      el["btn-review"].href = "review.html?token=" + encodeURIComponent(rt);
      PT.show(el["btn-review"], true);
    } else {
      PT.show(el["btn-review"], false);
    }

    showScreen("over");
  }

  /* ── Consent ──────────────────────────────────────────────────────────── */

  function showConsent(payload) {
    stopPolling();
    showScreen("consent");
    if (payload && payload.nickname) { el.nickname.value = payload.nickname; }
    /* Restore a previously used nickname from this device. In shared-card mode the
     * same student often plays twice, and retyping a nickname on a phone keyboard
     * in a queue is the point where people give up. */
    var remembered = PT.store.get("pt.nickname");
    if (!el.nickname.value && remembered) { el.nickname.value = remembered; }
    el.nickname.focus();
  }

  function submitConsent() {
    var nickname = (el.nickname.value || "").trim();
    PT.show(el["consent-error"], false);
    el["btn-consent"].disabled = true;

    PT.api.consent(token, nickname, PT.strings.meta.consentVersion).then(function (res) {
      el["btn-consent"].disabled = false;
      if (!res || !res.ok) {
        PT.setText(el["consent-error"], (res && res.message) || PT.t("errors.generic"));
        PT.show(el["consent-error"], true);
        return;
      }
      PT.store.set("pt.nickname", res.nickname || nickname);
      startGame();
      /* The first round is pushed by the server as a side effect of consent. Poll
       * once immediately in case the push was missed while the socket was still
       * joining its room. */
      pollSession();
    });
  }

  function declineConsent() {
    PT.show(el["consent-declined"], true);
    el["btn-consent"].disabled = true;
    el["btn-decline"].disabled = true;
    el.nickname.disabled = true;
  }

  /* ── Session lifecycle ────────────────────────────────────────────────── */

  function startGame() {
    showScreen("play");
    startPolling();
    connectSocket();
  }

  function connectSocket() {
    if (socket) { return; }
    socket = PT.createSocket(token, {
      onConnect: function () {
        socketLive = true;
        setConnBanner(null);
        /* Re-sync: anything emitted while disconnected is gone, and the socket
         * offers no replay. */
        pollSession();
      },
      onDisconnect: function () {
        socketLive = false;
        setConnBanner(PT.t("game.reconnecting"));
      },
      onError: function () {
        socketLive = false;
        setConnBanner(PT.t("game.reconnecting"));
      },
      events: {
        round_start:  function (d) { renderRound(d); },
        round_result: function (d) { showVerdict(d); },
        game_over:    function (d) { showGameOver(d); },
        error:        function (d) { setConnBanner((d && d.message) || PT.t("errors.generic")); },
      },
    });
    if (!socket) {
      /* No socket client at all. Polling carries the whole game, which is slower
       * to feel updates but fully functional. */
      socketLive = false;
      setConnBanner(PT.t("game.reconnecting"));
    }
  }

  /* Whether pushes are arriving. Kept as a function rather than read directly so
   * the flag has one definition site; it is used by the connection banner logic
   * below, which distinguishes "socket down, polling instead" from "connected". */
  function isSocketLive() { return socketLive; }

  function pollSession() {
    PT.api.session(token).then(function (res) {
      if (!res || !res.ok) {
        if (res && (res.err === "E_EXPIRED" || res.status === 404)) { fatal(res.message); }
        return;
      }
      /* The server answered, so the session is alive. Clear a stale "reconnecting"
       * banner if the socket has in fact come back — onConnect normally does this,
       * but a reconnect that happened between polls would otherwise leave the
       * banner up while everything was working, which reads as a broken game. */
      if (isSocketLive() && !el.conn.classList.contains("is-hidden")) {
        setConnBanner(null);
      }
      applySessionState(res);
    });
  }

  /* Reconcile a polled session payload with what is on screen.
   *
   * This is the fallback path, so it must be careful not to fight the socket. A
   * round that is already displayed is not re-rendered, because doing so would
   * restart the countdown and give the player extra time on every poll.
   */
  function applySessionState(s) {
    renderHud(s);

    if (s.state === "game_over" || s.state === "review") {
      showGameOver({ score: s.score, rounds_played: s.round, review_token: reviewToken });
      return;
    }
    if (s.state === "consent" || s.state === "awaiting_phone") {
      if (el["screen-consent"].classList.contains("is-hidden") &&
          el["screen-play"].classList.contains("is-hidden")) {
        showConsent(s);
      }
      return;
    }
    if (s.state === "expired") { fatal(PT.t("errors.E_EXPIRED")); return; }

    /* A live round. Only render if this is a round we have not shown yet, so a
     * poll arriving mid-round does not reset the timer. */
    if (s.current && s.current.item) {
      var roundNo = s.current.round;
      if (roundNo !== currentRound || el["screen-play"].classList.contains("is-hidden")) {
        if (el["screen-play"].classList.contains("is-hidden")) { showScreen("play"); }
        renderRound(s.current);
      }
    } else if (el["screen-play"].classList.contains("is-hidden")) {
      showScreen("play");
    }
  }

  function startPolling() {
    if (pollTimer !== null) { return; }
    pollTimer = window.setInterval(pollSession, POLL_MS);
  }

  function stopPolling() {
    if (pollTimer !== null) {
      window.clearInterval(pollTimer);
      pollTimer = null;
    }
  }

  /* ── Boot ─────────────────────────────────────────────────────────────── */

  function boot() {
    cacheElements();
    applyStrings();

    token = PT.qs("token");
    if (!token) {
      /* No token means the page was opened directly rather than from a QR code.
       * The PIN page is the supported way in without a scan, so send them there
       * rather than showing a dead end. */
      PT.setText(el["gone-text"], PT.t("join.noSession"));
      showScreen("gone");
      return;
    }

    showScreen("loading");

    PT.api.connect(token).then(function (res) {
      if (!res || !res.ok) {
        fatal(res && res.message ? res.message : PT.t("game.sessionGone"));
        return;
      }
      renderHud(res);
      if (res.needs_consent) {
        showConsent(res);
      } else {
        /* A returning player: consent is already on file, so go straight in and
         * let the poll pick up whatever round is live. */
        startGame();
        pollSession();
      }
    });

    el["btn-consent"].addEventListener("click", submitConsent);
    el["btn-decline"].addEventListener("click", declineConsent);
    el.nickname.addEventListener("keydown", function (e) {
      if (e.key === "Enter") { e.preventDefault(); submitConsent(); }
    });
    el["btn-phish"].addEventListener("click", function () { submitAnswer("phish"); });
    el["btn-safe"].addEventListener("click", function () { submitAnswer("safe"); });

    /* Keyboard play, for anyone who prefers it and for accessibility. The keys are
     * far apart on a keyboard so a mistap is unlikely. */
    document.addEventListener("keydown", function (e) {
      if (el["screen-play"].classList.contains("is-hidden")) { return; }
      if (e.target && /^(INPUT|TEXTAREA)$/.test(e.target.tagName)) { return; }
      if (e.key === "ArrowLeft" || e.key === "p" || e.key === "P") { submitAnswer("phish"); }
      else if (e.key === "ArrowRight" || e.key === "s" || e.key === "S") { submitAnswer("safe"); }
    });

    /* A backgrounded phone throttles timers hard, and on resume the countdown
     * would be badly wrong. Re-sync from the server instead of trusting the local
     * clock that has been frozen. */
    document.addEventListener("visibilitychange", function () {
      if (!document.hidden && !el["screen-play"].classList.contains("is-hidden")) {
        pollSession();
      }
    });
  }

  /* Exposed for the socket callback closure above. */
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", boot);
  } else {
    boot();
  }
})();
