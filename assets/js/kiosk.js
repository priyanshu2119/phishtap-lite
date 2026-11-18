/*
 * PhishTap Lite — kiosk page logic.
 *
 * WHY THIS POLLS *AND* LISTENS
 * Socket.IO pushes are the fast path and REST polling is the safety net, and both
 * are always active. That is deliberate redundancy, not indecision.
 *
 * A socket can be connected and still not be delivering: the room join can fail
 * silently, a proxy can buffer, or the brick can emit before the client has
 * subscribed. Every one of those looks identical from the page — nothing updates —
 * and diagnosing it live, in front of an audience, is not a good use of a demo.
 * Polling every two seconds means the page is correct within two seconds no matter
 * what the socket is doing, and the socket makes it feel instant when it works.
 *
 * The cost is one small JSON fetch every two seconds from a single client on a
 * LAN. That is nothing.
 */

/* global PT */
(function () {
  "use strict";

  var POLL_MS = 2000;
  var QR_SIZE = 240;

  /* Which view a session state maps to. Kept as an explicit table rather than a
   * chain of ifs so that adding a state server-side shows up here as an obvious
   * gap instead of silently falling through to the attract screen. */
  var VIEW_FOR_STATE = {
    awaiting_phone: "join",
    consent:        "join",   /* phone is open on the consent form; keep the QR up */
    round_start:    "play",
    awaiting_answer:"play",
    round_result:   "play",
    game_over:      "over",
    review:         "over",
    expired:        "attract",
  };

  var el = {};
  var last = null;            /* last snapshot applied */
  var lastQrUrl = null;       /* avoids re-rendering the QR every poll */
  var expiryTicker = null;
  var expiresAtLocal = 0;     /* Date.now() when the token dies, derived locally */
  var socket = null;
  var socketLive = false;

  function cacheElements() {
    var ids = [
      "link-dot", "link-text", "fault",
      "view-attract", "attract-title", "attract-tag", "attract-cta",
      "board-wrap", "board-title", "board-list",
      "view-join", "qr", "qr-url", "pin-label", "pin-digits", "pin-expiry", "join-hint",
      "join-fallback-label", "join-url",
      "view-play", "k-round", "v-round", "k-lives", "v-lives", "k-score", "v-score",
      "m-sender", "m-channel", "m-subject", "m-body", "play-who",
      "view-over", "over-eyebrow", "over-score", "over-who", "over-why",
      "over-board-wrap", "over-board-title", "over-board-list",
      "foot-left", "foot-right", "flash",
    ];
    for (var i = 0; i < ids.length; i++) { el[ids[i]] = PT.$(ids[i]); }
  }

  function applyStaticStrings() {
    PT.setText(el["attract-title"], PT.t("brand.name"));
    PT.setText(el["attract-tag"], PT.t("kiosk.attractHint"));
    PT.setText(el["attract-cta"], PT.t("kiosk.attractBody"));
    PT.setText(el["board-title"], PT.t("kiosk.leaderboardTitle"));
    PT.setText(el["over-board-title"], PT.t("kiosk.leaderboardTitle"));
    PT.setText(el["pin-label"], PT.t("kiosk.pinLabel"));
    PT.setText(el["join-hint"], PT.t("kiosk.scanToJoin"));
    PT.setText(el["join-fallback-label"], PT.t("kiosk.cantScan"));
    PT.setText(el["k-round"], PT.t("kiosk.roundLabel"));
    PT.setText(el["k-lives"], PT.t("kiosk.livesLabel"));
    PT.setText(el["k-score"], PT.t("kiosk.scoreLabel"));
    PT.setText(el["over-eyebrow"], PT.t("kiosk.gameOverTitle"));
    PT.setText(el["foot-left"], PT.t("kiosk.footerBaseUrl"));
  }

  /* ── Views ────────────────────────────────────────────────────────────── */

  function showView(name) {
    var views = ["attract", "join", "play", "over"];
    for (var i = 0; i < views.length; i++) {
      PT.show(el["view-" + views[i]], views[i] === name);
    }
  }

  function renderLink(snap) {
    var ok = !!snap.link_ok;
    el["link-dot"].className = "link-dot " + (ok ? "" : "down");
    PT.setText(el["link-text"], ok ? PT.t("admin.healthUp") : PT.t("admin.healthDown"));

    if (ok) {
      PT.show(el.fault, false);
    } else {
      PT.show(el.fault, true);
      el.fault.className = "banner banner-error fault-banner";
      PT.setText(el.fault, PT.t("kiosk.linkLost"));
    }
  }

  function renderFault(code) {
    PT.show(el.fault, true);
    el.fault.className = "banner banner-error fault-banner";
    PT.setText(el.fault, PT.t("kiosk.hwFault", { code: code || "unknown" }));
  }

  /* Hearts rather than a number, because three glyphs read instantly from the
   * back of a room and a digit does not. Clamped so a corrupt lives value cannot
   * print a 400-character string across the screen. */
  function hearts(n) {
    var count = Math.max(0, Math.min(9, Number(n) || 0));
    var out = "";
    for (var i = 0; i < count; i++) { out += "\u2665"; }
    return out || "\u2014";
  }

  function renderLeaderboard(rows, listEl) {
    PT.clear(listEl);
    if (!rows || !rows.length) {
      var empty = PT.make("div", "board-row muted center", PT.t("kiosk.leaderboardEmpty"));
      empty.style.gridColumn = "1 / -1";
      listEl.appendChild(empty);
      return;
    }
    for (var i = 0; i < rows.length; i++) {
      var row = PT.make("div", "board-row");
      row.appendChild(PT.make("div", "board-rank", String(i + 1)));
      /* setText, never innerHTML: nicknames are user-supplied and one of them
       * will eventually be <script>. */
      row.appendChild(PT.make("div", "board-name", rows[i].nickname || "?"));
      row.appendChild(PT.make("div", "board-score", String(rows[i].score || 0)));
      listEl.appendChild(row);
    }
  }

  function renderAttract(snap) {
    showView("attract");
    renderLeaderboard(snap.leaderboard, el["board-list"]);
  }

  function renderJoin(snap) {
    showView("join");
    var s = snap.session;

    PT.setText(el["pin-digits"], s.pin || "----");

    /* The QR is rendered only when the URL changes. Re-rendering every poll would
     * blank and redraw the canvas twice a second, which flickers badly enough that
     * a phone camera struggles to lock onto it. */
    var url = s.url || "";
    if (url && url !== lastQrUrl) {
      lastQrUrl = url;
      var drew = PT.renderQr(el.qr, url, QR_SIZE);
      if (!drew) {
        /* No QR library, or it threw. The URL as text is the fallback, and it is
         * always shown anyway because DHCP can change the board's address and a
         * code that cannot be scanned should still be typeable. */
        PT.clear(el.qr);
        el.qr.appendChild(PT.make("div", "qr-fallback", url));
      }
    }
    PT.setText(el["qr-url"], url);

    /* The PIN page address, built from the same base the QR used. Derived from
     * the session URL rather than window.location so it stays correct if the page
     * is being viewed through a proxy or a different host than the board reports. */
    if (url) {
      var joinUrl = url.split("/game.html")[0] + "/join.html";
      PT.setText(el["join-url"], joinUrl);
      PT.show(el["join-url"], true);
      PT.show(el["join-fallback-label"], true);
    }

    startExpiry(s.expires_in_s);
  }

  /* The TTL counts down locally from the server's remaining-seconds figure. The
   * server sends a duration rather than a deadline because the board has no RTC
   * and the phone's clock is unrelated to it; a local countdown from a duration is
   * the only thing that is actually correct. */
  function startExpiry(secondsLeft) {
    if (typeof secondsLeft !== "number" || secondsLeft <= 0) {
      stopExpiry();
      PT.setText(el["pin-expiry"], "");
      return;
    }
    expiresAtLocal = Date.now() + secondsLeft * 1000;
    if (expiryTicker !== null) { return; }   /* already ticking; just re-anchored */
    expiryTicker = window.setInterval(tickExpiry, 250);
    tickExpiry();
  }

  function stopExpiry() {
    if (expiryTicker !== null) {
      window.clearInterval(expiryTicker);
      expiryTicker = null;
    }
  }

  function tickExpiry() {
    var left = Math.max(0, Math.round((expiresAtLocal - Date.now()) / 1000));
    PT.setText(el["pin-expiry"], PT.t("kiosk.sessionExpires", { seconds: left }));
    if (left <= 0) { stopExpiry(); }
  }

  function renderPlay(snap) {
    showView("play");
    stopExpiry();
    var s = snap.session;

    /* Round is zero-based server-side; people count from one. */
    PT.setText(el["v-round"], String((Number(s.round) || 0) + 1));
    PT.setText(el["v-lives"], hearts(s.lives));
    PT.setText(el["v-score"], String(s.score || 0));
    PT.setText(el["play-who"], s.nickname
      ? PT.t("kiosk.playerJoined", { nickname: s.nickname })
      : "");

    var cur = s.current;
    var hasMsg = !!(cur && cur.item);
    PT.show(el["m-sender"], hasMsg);
    PT.show(el["m-channel"], hasMsg);
    PT.show(el["m-subject"], hasMsg && !!cur.item.subject);
    PT.show(el["m-body"], hasMsg);

    if (hasMsg) {
      var item = cur.item;
      PT.setText(el["m-sender"], item.sender || "");
      PT.setText(el["m-channel"], channelName(item.channel));
      PT.setText(el["m-subject"], item.subject || "");
      /* setText only. The body contains lookalike domains and fake links by
       * design; rendering any of it as HTML or as an anchor would make the kiosk
       * the one place in the project where a phishing link is actually clickable. */
      PT.setText(el["m-body"], item.body || "");
    }
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

  function renderOver(snap) {
    showView("over");
    stopExpiry();
    var s = snap.session;
    PT.setText(el["over-score"], String(s.score || 0));
    PT.setText(el["over-who"], PT.t("kiosk.finalScore", {
      nickname: s.nickname || "?", score: s.score || 0,
    }));
    PT.setText(el["over-why"], PT.t("kiosk.reviewHint"));
    renderLeaderboard(snap.leaderboard, el["over-board-list"]);
  }

  /* ── Flash ──────────────────────────────────────────────────────────────
   * The kiosk's round_result push is a plain snapshot with no correctness field,
   * so the outcome is inferred from the score and lives deltas. Both are already
   * in the payload and both are reliable: a wrong answer scores zero and a lost
   * life always decrements. Inferring beats adding a field here, because the
   * snapshot is also what the phone-less audience sees and it should stay a pure
   * description of state rather than a description of the last event.
   */
  function flashFor(prev, next) {
    if (!prev || !next || !prev.session || !next.session) { return; }
    var p = prev.session, n = next.session;
    var gained = (Number(n.score) || 0) - (Number(p.score) || 0);
    var lost = (Number(p.lives) || 0) - (Number(n.lives) || 0);

    var kind = null;
    if (lost > 0) { kind = "bad"; }
    else if (gained > 0) { kind = "good"; }
    if (!kind) { return; }

    el.flash.className = "flash-overlay";
    /* Force a reflow so re-applying the same class restarts the animation. Without
     * this, two correct answers in a row would flash once. */
    void el.flash.offsetWidth;
    el.flash.className = "flash-overlay " + kind;
  }

  /* ── Apply a snapshot ─────────────────────────────────────────────────── */

  function apply(snap) {
    if (!snap || typeof snap !== "object") { return; }

    var prev = last;
    last = snap;

    renderLink(snap);

    var s = snap.session;
    var view = (!s || s.state === "expired") ? "attract" : (VIEW_FOR_STATE[s.state] || "attract");

    /* An unknown state falls back to attract, which is the safe choice: showing
     * a stale game screen for a state this page has never heard of would be worse
     * than showing the idle screen. */
    if (s && !VIEW_FOR_STATE[s.state]) {
      if (window.console) { console.warn("kiosk: unknown session state", s.state); }
    }

    if (view === "attract") { renderAttract(snap); }
    else if (view === "join") { renderJoin(snap); }
    else if (view === "play") { renderPlay(snap); }
    else if (view === "over") { renderOver(snap); }

    if (view !== "join") { lastQrUrl = null; }

    flashFor(prev, snap);
    renderFooter(snap);
  }

  function renderFooter(snap) {
    var base = snap.base_url || window.location.origin;
    PT.setText(el["foot-right"], base);

    var mcu = snap.mcu || {};
    var bits = [];
    if (mcu.fw) { bits.push("fw " + mcu.fw); }
    if (mcu.rfid_version !== undefined && mcu.rfid_version !== null) {
      var hex = Number(mcu.rfid_version).toString(16).toUpperCase();
      bits.push("rfid 0x" + (hex.length < 2 ? "0" + hex : hex));
    }
    if (typeof snap.seconds_since_mcu === "number") {
      bits.push("mcu " + snap.seconds_since_mcu + "s");
    }
    PT.setText(el["foot-left"],
      PT.t("kiosk.footerBaseUrl") + (bits.length ? "  \u00b7  " + bits.join("  \u00b7  ") : "")
      /* Surfacing whether the socket is live is worth the few pixels. Both paths
       * render the same content, so without this there is no way to tell from the
       * page whether pushes are arriving or whether polling is silently carrying
       * the whole thing — and that distinction is the first thing to check when
       * updates seem slow. */
      + "  \u00b7  " + (socketLive ? "socket live" : "polling"));
  }

  /* ── Data ─────────────────────────────────────────────────────────────── */

  function poll() {
    PT.api.kiosk().then(function (res) {
      if (res && res.ok) { apply(res); }
    });
  }

  function connectSocket() {
    socket = PT.createSocket("kiosk", {
      onConnect: function () {
        socketLive = true;
        /* Re-sync immediately: events emitted while disconnected were missed and
         * the socket gives no replay. */
        poll();
      },
      onDisconnect: function () { socketLive = false; },
      onError: function () { socketLive = false; },
      events: {
        kiosk_state:    function (d) { apply(d); },
        session_started:function (d) { apply(d); },
        round_start:    function (d) { apply(d); },
        round_result:   function (d) { apply(d); },
        game_over:      function (d) { apply(d); },
        hw_fault:       function (d) { renderFault(d && d.code); },
      },
    });
    if (!socket) {
      /* No socket.io client at all. Polling alone is a fully supported mode. */
      socketLive = false;
    }
  }

  function init() {
    cacheElements();
    applyStaticStrings();
    showView("attract");
    PT.setText(el["link-text"], PT.t("common.loading"));

    poll();
    connectSocket();
    window.setInterval(poll, POLL_MS);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
