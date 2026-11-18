/*
 * PhishTap Lite — admin panel logic.
 *
 * AUTHENTICATION
 * The admin token goes in an X-Admin-Token header on every request and is stored
 * in sessionStorage, not localStorage. Closing the tab ends the session, which is
 * the right behaviour for a panel left open on a shared demo laptop: the token
 * grants database deletion, so it should not survive the week.
 *
 * DESTRUCTIVE ACTIONS
 * The database reset requires typing the literal word RESET, matching the server's
 * own check. A confirm dialog alone is not enough, because confirm dialogs get
 * clicked through reflexively and this one erases every record the project has
 * collected. Making the operator type a word is friction on purpose.
 *
 * RENDERING
 * Every value from the server goes through setText or createTextNode. Nicknames
 * are user-supplied and player codes are derived from them, so treating any of it
 * as HTML would be a stored XSS in the one page that can delete the database.
 */

/* global PT */
(function () {
  "use strict";

  var TOKEN_KEY = "pt.admin_token";
  var el = {};
  var token = null;
  var lastAnalytics = null;

  function cacheElements() {
    var ids = [
      "screen-login", "login-title", "login-body", "pin", "pin-label", "login-error", "btn-login",
      "screen-panel", "panel-title", "btn-logout", "panel-error", "panel-ok",
      "health-title", "health-kv",
      "cohort-title", "cohort-grid", "cohort-labels",
      "cue-title", "cue-hint", "cue-head", "cue-body",
      "reaction-title", "reaction-hint", "reaction-head", "reaction-body", "reaction-excluded",
      "funnel-title", "funnel-hint", "funnel-grid",
      "export-title", "export-hint", "btn-export",
      "danger-title", "player-delete-label", "player-id", "btn-delete-player",
      "clock-label", "clock-body", "clock-offset", "btn-clock", "clock-current",
      "reset-title", "reset-body", "reset-confirm", "btn-reset",
    ];
    for (var i = 0; i < ids.length; i++) { el[ids[i]] = PT.$(ids[i]); }
  }

  function applyStrings() {
    PT.setText(el["login-title"], PT.t("admin.loginTitle"));
    PT.setText(el["login-body"], PT.t("admin.loginBody"));
    PT.setText(el["pin-label"], PT.t("admin.pinLabel"));
    PT.setText(el["btn-login"], PT.t("admin.login"));
    PT.setText(el["panel-title"], PT.t("admin.title"));
    PT.setText(el["btn-logout"], PT.t("admin.logout"));
    PT.setText(el["health-title"], PT.t("admin.healthTitle"));
    PT.setText(el["cohort-title"], PT.t("admin.cohortTitle"));
    PT.setText(el["cue-title"], PT.t("admin.cueTitle"));
    PT.setText(el["cue-hint"], PT.t("admin.cueHint"));
    PT.setText(el["reaction-title"], PT.t("admin.reactionTitle"));
    PT.setText(el["reaction-hint"], PT.t("admin.reactionHint"));
    PT.setText(el["funnel-title"], PT.t("admin.funnelTitle"));
    PT.setText(el["funnel-hint"], PT.t("admin.funnelHint"));
    PT.setText(el["export-title"], PT.t("admin.exportButton"));
    PT.setText(el["export-hint"], PT.t("admin.exportHint"));
    PT.setText(el["danger-title"], PT.t("admin.sectionDanger"));
    PT.setText(el["player-delete-label"], PT.t("admin.playerDeleteLabel"));
    PT.setText(el["btn-delete-player"], PT.t("admin.playerDeleteButton"));
    PT.setText(el["clock-label"], PT.t("admin.clockLabel"));
    PT.setText(el["clock-body"], PT.t("admin.clockBody"));
    PT.setText(el["btn-clock"], PT.t("admin.clockSet"));
    PT.setText(el["reset-title"], PT.t("admin.resetTitle"));
    PT.setText(el["reset-body"], PT.t("admin.resetBody"));
    PT.setText(el["btn-reset"], PT.t("admin.resetButton"));
    el["player-id"].placeholder = PT.t("admin.playerDeletePlaceholder");
  }

  /* ── Small render helpers ─────────────────────────────────────────────── */

  function pct(v) {
    if (v === null || v === undefined) { return PT.t("common.none"); }
    return (Math.round(v * 1000) / 10) + "%";
  }

  function num(v) {
    return (v === null || v === undefined) ? PT.t("common.none") : String(v);
  }

  function kvRow(dt, dd) {
    var k = PT.make("dt", null, dt);
    var v = PT.make("dd", null, dd);
    return [k, v];
  }

  function metric(key, value) {
    var box = PT.make("div", "metric");
    box.appendChild(PT.make("div", "metric-k", key));
    box.appendChild(PT.make("div", "metric-v", value));
    return box;
  }

  function flashOk(message) {
    PT.setText(el["panel-ok"], message);
    PT.show(el["panel-ok"], true);
    PT.show(el["panel-error"], false);
    window.setTimeout(function () { PT.show(el["panel-ok"], false); }, 4000);
  }

  function flashError(message) {
    PT.setText(el["panel-error"], message);
    PT.show(el["panel-error"], true);
    PT.show(el["panel-ok"], false);
  }

  /* ── Auth ─────────────────────────────────────────────────────────────── */

  function showLogin() {
    PT.show(el["screen-panel"], false);
    PT.show(el["screen-login"], true);
    el.pin.value = "";
    el.pin.focus();
  }

  function showPanel() {
    PT.show(el["screen-login"], false);
    PT.show(el["screen-panel"], true);
    refresh();
  }

  function login() {
    var pin = (el.pin.value || "").trim();
    if (!pin) { return; }
    el["btn-login"].disabled = true;
    PT.show(el["login-error"], false);

    PT.api.adminLogin(pin).then(function (res) {
      el["btn-login"].disabled = false;
      if (!res || !res.ok || !res.admin_token) {
        PT.setText(el["login-error"], (res && res.message) || PT.t("errors.E_PIN"));
        PT.show(el["login-error"], true);
        el.pin.select();
        return;
      }
      token = res.admin_token;
      PT.store.set(TOKEN_KEY, token);
      showPanel();
    });
  }

  function logout() {
    token = null;
    PT.store.remove(TOKEN_KEY);
    showLogin();
  }

  /* A 401 from any admin call means the token expired (30 minute TTL) or the
   * backend restarted. Dropping back to the login screen is better than leaving
   * the operator clicking buttons that silently fail. */
  function handleAuthFailure(res) {
    if (res && res.status === 401) {
      logout();
      return true;
    }
    return false;
  }

  /* ── Data ─────────────────────────────────────────────────────────────── */

  function refresh() {
    PT.api.health().then(renderHealth);
    PT.api.adminAnalytics(token).then(function (res) {
      if (handleAuthFailure(res)) { return; }
      if (!res || !res.ok) {
        flashError((res && res.message) || PT.t("errors.generic"));
        return;
      }
      lastAnalytics = res;
      renderCohort(res.cohort || {});
      renderCues(res.by_cue || []);
      renderReaction(res.reaction_ms || {});
      renderFunnel(res.funnel || {});
    });
  }

  function renderHealth(h) {
    PT.clear(el["health-kv"]);
    if (!h || !h.ok) {
      var pair = kvRow(PT.t("admin.healthBridge"), PT.t("admin.healthDown"));
      el["health-kv"].appendChild(pair[0]);
      el["health-kv"].appendChild(pair[1]);
      return;
    }
    var rows = [
      [PT.t("admin.healthBridge"), h.bridge_ok ? PT.t("admin.healthUp") : PT.t("admin.healthDown")],
      [PT.t("admin.healthVersion"), (h.version || "?") + " (proto " + (h.proto || "?") + ")"],
      [PT.t("admin.healthUptime"), (h.uptime_s !== undefined ? h.uptime_s + " s" : "?")],
      [PT.t("admin.healthBaseUrl"), h.base_url || window.location.origin],
    ];
    for (var i = 0; i < rows.length; i++) {
      var p = kvRow(rows[i][0], rows[i][1]);
      el["health-kv"].appendChild(p[0]);
      el["health-kv"].appendChild(p[1]);
    }
  }

  function renderCohort(c) {
    PT.clear(el["cohort-grid"]);
    var cards = [
      [PT.t("admin.cohortTotal"), num(c.total_answers)],
      [PT.t("admin.cohortAccuracy"), pct(c.accuracy)],
      [PT.t("admin.cohortTimeouts"), num(c.timeouts) + "  (" + pct(c.timeout_rate) + ")"],
    ];
    for (var i = 0; i < cards.length; i++) {
      el["cohort-grid"].appendChild(metric(cards[i][0], cards[i][1]));
    }

    /* The phish/safe split is the number that matters. A student who answers
     * "phish" to everything scores 50% on a balanced corpus while learning
     * nothing, so a single accuracy figure can hide exactly the behaviour the
     * tool exists to correct. */
    PT.clear(el["cohort-labels"]);
    var byLabel = c.by_label || {};
    var kinds = [["phish", PT.t("admin.cohortPhish")], ["safe", PT.t("admin.cohortSafe")]];
    for (var k = 0; k < kinds.length; k++) {
      var entry = byLabel[kinds[k][0]] || {};
      var box = PT.make("div", "metric");
      box.appendChild(PT.make("div", "metric-k", kinds[k][1]));
      box.appendChild(PT.make("div", "metric-v", pct(entry.accuracy)));
      box.appendChild(PT.make("div", "tiny faint",
        (entry.correct || 0) + " / " + (entry.seen || 0)));
      var track = PT.make("div", "bar-track");
      var fill = PT.make("div", "bar-fill");
      fill.style.width = Math.max(0, Math.min(100, (entry.accuracy || 0) * 100)) + "%";
      fill.style.background = kinds[k][0] === "phish" ? "var(--phish)" : "var(--safe)";
      track.appendChild(fill);
      box.appendChild(track);
      el["cohort-labels"].appendChild(box);
    }
    el["cohort-labels"].className = "metric-grid";
  }

  function renderCues(rows) {
    PT.clear(el["cue-head"]);
    PT.clear(el["cue-body"]);

    var headers = PT.t("admin.cueHeader");
    for (var h = 0; h < headers.length; h++) {
      var th = PT.make("th", h === 0 ? "" : "num", headers[h]);
      el["cue-head"].appendChild(th);
    }

    if (!rows.length) {
      var tr = PT.make("tr");
      var td = PT.make("td", "muted", PT.t("admin.noData"));
      td.colSpan = headers.length;
      tr.appendChild(td);
      el["cue-body"].appendChild(tr);
      return;
    }

    var names = (PT.strings.review && PT.strings.review.cueNames) || {};
    for (var i = 0; i < rows.length; i++) {
      var r = rows[i];
      var row = PT.make("tr");
      /* setText for the cue name: it comes from corpus.json, which a human edits.
       * An angle bracket in a cue label must not become markup. */
      row.appendChild(PT.make("td", null, names[r.cue] || r.cue));
      row.appendChild(PT.make("td", "num", num(r.seen)));
      row.appendChild(PT.make("td", "num", num(r.correct)));
      var acc = PT.make("td", "num", pct(r.accuracy));
      /* Colour the accuracy so the weak cues are findable at a glance, which is
       * the entire reason this table exists. */
      if (typeof r.accuracy === "number") {
        acc.style.color = r.accuracy < 0.5 ? "var(--phish)"
                        : (r.accuracy < 0.75 ? "var(--warn)" : "var(--safe)");
        acc.style.fontWeight = "700";
      }
      row.appendChild(acc);
      row.appendChild(PT.make("td", "num", num(r.timeouts)));
      el["cue-body"].appendChild(row);
    }
  }

  function renderReaction(rt) {
    PT.clear(el["reaction-head"]);
    PT.clear(el["reaction-body"]);

    var headers = [
      PT.t("admin.reactionSource"), PT.t("admin.reactionN"), PT.t("admin.reactionMin"),
      PT.t("admin.reactionP50"), PT.t("admin.reactionP90"), PT.t("admin.reactionMax"),
    ];
    for (var h = 0; h < headers.length; h++) {
      el["reaction-head"].appendChild(PT.make("th", h === 0 ? "" : "num", headers[h]));
    }

    var bySource = rt.by_source || {};
    var sources = Object.keys(bySource).sort();
    if (!sources.length) {
      var tr = PT.make("tr");
      var td = PT.make("td", "muted", PT.t("admin.noData"));
      td.colSpan = headers.length;
      tr.appendChild(td);
      el["reaction-body"].appendChild(tr);
    } else {
      for (var i = 0; i < sources.length; i++) {
        var s = bySource[sources[i]];
        var row = PT.make("tr");
        row.appendChild(PT.make("td", null, sources[i]));
        row.appendChild(PT.make("td", "num", num(s.n)));
        row.appendChild(PT.make("td", "num", num(s.min_ms)));
        row.appendChild(PT.make("td", "num", num(s.p50_ms)));
        row.appendChild(PT.make("td", "num", num(s.p90_ms)));
        row.appendChild(PT.make("td", "num", num(s.max_ms)));
        el["reaction-body"].appendChild(row);
      }
    }

    PT.setText(el["reaction-excluded"], rt.excluded_ack_missing
      ? PT.t("admin.reactionExcluded", { count: rt.excluded_ack_missing })
      : "");
  }

  function renderFunnel(f) {
    PT.clear(el["funnel-grid"]);
    var cards = [
      [PT.t("admin.funnelTaps"), num(f.taps)],
      [PT.t("admin.funnelCreated"), num(f.sessions_created) + "  (" + pct(f.tap_to_create) + ")"],
      [PT.t("admin.funnelStarted"), num(f.sessions_started) + "  (" + pct(f.create_to_start) + ")"],
      [PT.t("admin.funnelCompleted"), num(f.sessions_completed) + "  (" + pct(f.start_to_complete) + ")"],
      [PT.t("admin.funnelExpired"), num(f.sessions_expired)],
    ];
    for (var i = 0; i < cards.length; i++) {
      el["funnel-grid"].appendChild(metric(cards[i][0], cards[i][1]));
    }
  }

  /* ── Actions ──────────────────────────────────────────────────────────── */

  function doExport() {
    el["btn-export"].disabled = true;
    PT.api.adminExportUrl(token).then(function (url) {
      el["btn-export"].disabled = false;
      /* Trigger the download through a temporary anchor. The blob URL is revoked
       * afterwards so repeated exports do not accumulate memory. */
      var a = document.createElement("a");
      a.href = url;
      a.download = "phishtap-export.csv";
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
      window.setTimeout(function () { window.URL.revokeObjectURL(url); }, 1000);
      flashOk(PT.t("admin.saved"));
    }, function () {
      el["btn-export"].disabled = false;
      flashError(PT.t("errors.generic"));
    });
  }

  function doDeletePlayer() {
    var pid = (el["player-id"].value || "").trim();
    if (!pid) { return; }
    if (!window.confirm(PT.t("admin.playerDeleteLabel") + ": " + pid + "?")) { return; }

    el["btn-delete-player"].disabled = true;
    PT.api.adminDeletePlayer(token, pid).then(function (res) {
      el["btn-delete-player"].disabled = false;
      if (handleAuthFailure(res)) { return; }
      if (!res || !res.ok) { flashError((res && res.message) || PT.t("errors.generic")); return; }
      el["player-id"].value = "";
      flashOk(PT.t("admin.saved"));
      refresh();
    });
  }

  function doSetClock() {
    var raw = (el["clock-offset"].value || "").trim();
    if (raw === "") { return; }
    var ms = Number(raw);
    if (!isFinite(ms)) { flashError(PT.t("errors.generic")); return; }

    el["btn-clock"].disabled = true;
    PT.api.adminClock(token, Math.round(ms)).then(function (res) {
      el["btn-clock"].disabled = false;
      if (handleAuthFailure(res)) { return; }
      if (!res || !res.ok) { flashError((res && res.message) || PT.t("errors.generic")); return; }
      PT.setText(el["clock-current"],
        PT.t("admin.clockCurrent") + ": " + res.clock_offset_ms + " ms");
      flashOk(PT.t("admin.saved"));
    });
  }

  function doReset() {
    var confirmText = (el["reset-confirm"].value || "").trim();
    /* Checked client-side as well as server-side. The server is authoritative, but
     * letting a mistyped confirmation reach it and come back as an error is a
     * worse experience than saying so immediately. */
    if (confirmText !== "RESET") {
      flashError(PT.t("admin.resetBody"));
      return;
    }
    if (!window.confirm(PT.t("admin.resetTitle") + "?")) { return; }

    el["btn-reset"].disabled = true;
    PT.api.adminDbReset(token, confirmText).then(function (res) {
      el["btn-reset"].disabled = false;
      if (handleAuthFailure(res)) { return; }
      if (!res || !res.ok) { flashError((res && res.message) || PT.t("errors.generic")); return; }
      el["reset-confirm"].value = "";
      flashOk(PT.t("admin.saved"));
      refresh();
    });
  }

  /* ── Boot ─────────────────────────────────────────────────────────────── */

  function boot() {
    cacheElements();
    applyStrings();

    el["btn-login"].addEventListener("click", login);
    el.pin.addEventListener("keydown", function (e) {
      if (e.key === "Enter") { e.preventDefault(); login(); }
    });
    el["btn-logout"].addEventListener("click", logout);
    el["btn-export"].addEventListener("click", doExport);
    el["btn-delete-player"].addEventListener("click", doDeletePlayer);
    el["btn-clock"].addEventListener("click", doSetClock);
    el["btn-reset"].addEventListener("click", doReset);

    var stored = PT.store.get(TOKEN_KEY);
    if (stored) {
      /* A stored token may be expired. Rather than assuming it is good, use it for
       * the first analytics call and let a 401 send us back to the login screen. */
      token = stored;
      showPanel();
    } else {
      showLogin();
    }

    /* Refresh on a slow interval so the panel stays useful while a demo runs,
     * without hammering the board. Ten seconds is plenty for reporting. */
    window.setInterval(function () {
      if (token && !el["screen-panel"].classList.contains("is-hidden")) { refresh(); }
    }, 10000);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", boot);
  } else {
    boot();
  }
})();
