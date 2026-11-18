/*
 * PhishTap Lite — shared browser helpers.
 *
 * Loaded by every page before its own script. Defines one global, `PT`.
 *
 * THREE THINGS THIS FILE IS STRICT ABOUT, AND WHY
 *
 * 1. Escaping. Every piece of corpus text is attacker-shaped by design: the whole
 *    point of the project is to show students messages containing lookalike
 *    domains, fake sender names and malicious-looking links. Those strings are
 *    rendered as TEXT, never as HTML, and never as anchors. `setText` is the only
 *    sanctioned way to put server data into the DOM; `setHtml` exists solely for
 *    markup this file builds itself and refuses anything else.
 *
 * 2. The socket URL. Arduino's own `arduino.js` helper hardcodes
 *    `io(\`http://${window.location.host}\`)`. That breaks the moment the brick
 *    runs with use_tls=True, because a page served over HTTPS cannot open a
 *    plaintext WebSocket — browsers block it as mixed content and the socket
 *    silently never connects. This file derives the scheme from
 *    `window.location.protocol` instead, so it works either way.
 *
 * 3. No external requests. Everything is same-origin and relative. No CDN, no
 *    font, no analytics beacon. docs/ARCHITECTURE.md §7 requires it, and the demo
 *    runs offline anyway, so an external request would just be a hang.
 */

/* global io, QRCode, PT_STRINGS */
/* exported PT */

(function () {
  "use strict";

  var STRINGS = window.PT_STRINGS || {};

  /* ── Strings ────────────────────────────────────────────────────────────
   * `t("kiosk.pinLabel")` walks the dotted path. A missing key returns the key
   * itself rather than undefined, because a visible "kiosk.pinLabel" on screen is
   * a bug someone will report, whereas "undefined" looks like a data problem and
   * sends you looking in the wrong place.
   */
  function t(path, vars) {
    var parts = String(path).split(".");
    var node = STRINGS;
    for (var i = 0; i < parts.length; i++) {
      if (node === null || node === undefined) { return path; }
      node = node[parts[i]];
    }
    if (typeof node !== "string") { return path; }
    if (!vars) { return node; }
    return node.replace(/\{(\w+)\}/g, function (whole, key) {
      return Object.prototype.hasOwnProperty.call(vars, key) ? String(vars[key]) : whole;
    });
  }

  /* Map a server E_* code to a human sentence. Unknown codes fall back to the
   * generic message, so adding a code server-side never shows a raw constant to a
   * student. A server-supplied `message` is preferred when present, since it can
   * carry specifics like how many PIN attempts remain. */
  function errorMessage(errCode, serverMessage) {
    if (serverMessage) { return serverMessage; }
    var table = STRINGS.errors || {};
    return table[errCode] || table.generic || "Something went wrong.";
  }

  /* ── DOM helpers ──────────────────────────────────────────────────────── */

  function $(id) { return document.getElementById(id); }

  /* The ONLY sanctioned way to put server-controlled data on screen.
   *
   * There is deliberately no setHtml counterpart. An earlier revision exported
   * one "for markup this file builds itself", and nothing ever called it. Shipping
   * an unused HTML-injection sink from the shared library, in a project whose
   * entire content is attacker-shaped strings, is an invitation: the next person
   * who needs bold text reaches for it instead of building elements. tools/web_gate.sh
   * fails the build if innerHTML reappears outside this file.
   */
  function setText(el, text) {
    if (!el) { return; }
    el.textContent = (text === null || text === undefined) ? "" : String(text);
  }

  function show(el, visible) {
    if (!el) { return; }
    el.hidden = !visible;
    if (visible) { el.classList.remove("is-hidden"); }
    else { el.classList.add("is-hidden"); }
  }

  function clear(el) {
    if (!el) { return; }
    while (el.firstChild) { el.removeChild(el.firstChild); }
  }

  function make(tag, className, text) {
    var el = document.createElement(tag);
    if (className) { el.className = className; }
    if (text !== undefined && text !== null) { el.textContent = String(text); }
    return el;
  }

  /* ── Query string ───────────────────────────────────────────────────────
   * The API uses query strings only, never path parameters, so the pages do too.
   */
  function qs(name) {
    var m = new RegExp("[?&]" + name.replace(/[.*+?^${}()|[\]\\]/g, "\\$&") + "=([^&#]*)")
      .exec(window.location.search);
    return m ? decodeURIComponent(m[1].replace(/\+/g, " ")) : "";
  }

  function buildQuery(params) {
    var out = [];
    for (var k in params) {
      if (!Object.prototype.hasOwnProperty.call(params, k)) { continue; }
      var v = params[k];
      if (v === null || v === undefined || v === "") { continue; }
      out.push(encodeURIComponent(k) + "=" + encodeURIComponent(String(v)));
    }
    return out.length ? "?" + out.join("&") : "";
  }

  /* ── REST client ────────────────────────────────────────────────────────
   * The server envelope is flat: `_ok()` merges the payload into the top level
   * alongside `ok: true`, and `_err()` returns `{ok: false, err, message}`.
   * There is no `data` wrapper. Verified against python/phishtap/api.py.
   *
   * Every call resolves to a uniform {ok, status, ...} object rather than
   * throwing, because almost every caller wants to show a message and carry on.
   * A thrown exception on a 404 would mean try/catch around every fetch.
   */
  function request(method, path, params, body) {
    var url = path + (params ? buildQuery(params) : "");
    var init = { method: method, cache: "no-store", headers: {} };
    if (body !== undefined && body !== null) {
      init.headers["Content-Type"] = "application/json";
      init.body = JSON.stringify(body);
    }

    return fetch(url, init).then(function (res) {
      return res.text().then(function (raw) {
        var parsed = null;
        if (raw) {
          try { parsed = JSON.parse(raw); }
          catch (e) { parsed = null; }
        }
        if (!parsed || typeof parsed !== "object") {
          return {
            ok: false, status: res.status, err: "E_INTERNAL",
            message: t("errors.generic"),
          };
        }
        parsed.status = res.status;
        /* A non-2xx with ok:true would be a server bug; trust the status. */
        if (!res.ok) { parsed.ok = false; }
        if (parsed.ok === false && !parsed.message) {
          parsed.message = errorMessage(parsed.err);
        }
        return parsed;
      });
    }, function () {
      /* Network failure, DNS, CORS, board unreachable. Distinct from an HTTP
       * error, and the message says so, because "cannot reach the console" tells
       * a student to check the Wi-Fi while "session expired" tells them to tap
       * the card again. Conflating them sends people down the wrong path. */
      return {
        ok: false, status: 0, err: "E_NETWORK",
        message: t("errors.network"),
      };
    });
  }

  var api = {
    get: function (path, params) { return request("GET", path, params, null); },
    post: function (path, body, params) { return request("POST", path, params, body || {}); },

    /* Named endpoints, so a path typo is one place to fix and a page cannot
     * quietly invent its own. */
    health: function () { return api.get("/api/health"); },
    kiosk: function () { return api.get("/api/kiosk"); },
    leaderboard: function () { return api.get("/api/leaderboard"); },
    session: function (token) { return api.get("/api/session", { token: token }); },
    review: function (token) { return api.get("/api/review", { token: token }); },
    connect: function (token) { return api.post("/api/session/connect", { token: token }); },
    join: function (pin) { return api.post("/api/join", { pin: pin }); },
    consent: function (token, nickname, version) {
      return api.post("/api/consent", {
        token: token, nickname: nickname, consent_version: version,
      });
    },
    renderAck: function (token) { return api.post("/api/render_ack", { token: token }); },
    answer: function (token, answer, source) {
      return api.post("/api/answer", { token: token, answer: answer, source: source || "phone" });
    },
    deleteRequest: function (token) { return api.post("/api/delete_request", { token: token }); },
    adminLogin: function (pin) { return api.post("/api/admin/login", { pin: pin }); },

    /* Admin endpoints take the token as a header. These are thin wrappers so a
     * page cannot accidentally send the token in the query string instead. */
    adminAnalytics: function (token) {
      return adminRequest("GET", "/api/admin/analytics", token, null, null);
    },
    adminDeletePlayer: function (token, playerId) {
      return adminRequest("POST", "/api/admin/player/delete", token, { player_id: playerId });
    },
    adminDbReset: function (token, confirmText) {
      return adminRequest("POST", "/api/admin/db/reset", token, { confirm: confirmText });
    },
    adminClock: function (token, offsetMs) {
      return adminRequest("POST", "/api/admin/clock", token, { offset_ms: offsetMs });
    },
    /* The CSV export is a download, not JSON, so it navigates rather than
     * fetching. The token still goes in the header, which a plain navigation
     * cannot set, so this uses fetch and turns the body into an object URL. */
    adminExportUrl: function (token) {
      return fetch("/api/admin/export", {
        method: "GET", cache: "no-store",
        headers: token ? { "X-Admin-Token": token } : {},
      }).then(function (res) {
        if (!res.ok) { throw new Error("export failed: HTTP " + res.status); }
        return res.blob();
      }).then(function (blob) {
        return window.URL.createObjectURL(blob);
      });
    },
  };

  /* Admin endpoints authenticate with a header, not a query parameter. A token in
   * a URL ends up in access logs, browser history and Referer headers, which is a
   * needless way to leak something that grants database deletion.
   */
  function adminRequest(method, path, adminToken, body, params) {
    var url = path + (params ? buildQuery(params) : "");
    var init = { method: method, cache: "no-store", headers: {} };
    if (adminToken) { init.headers["X-Admin-Token"] = adminToken; }
    if (body !== undefined && body !== null) {
      init.headers["Content-Type"] = "application/json";
      init.body = JSON.stringify(body);
    }
    return fetch(url, init).then(function (res) {
      return res.text().then(function (raw) {
        var parsed = null;
        try { parsed = raw ? JSON.parse(raw) : null; } catch (e) { parsed = null; }
        if (!parsed || typeof parsed !== "object") {
          return { ok: false, status: res.status, err: "E_INTERNAL", message: t("errors.generic") };
        }
        parsed.status = res.status;
        if (!res.ok) { parsed.ok = false; }
        if (parsed.ok === false && !parsed.message) { parsed.message = errorMessage(parsed.err); }
        return parsed;
      });
    }, function () {
      return { ok: false, status: 0, err: "E_NETWORK", message: t("errors.network") };
    });
  }

  /* ── Socket.IO ──────────────────────────────────────────────────────────
   * Rooms are joined with the brick's built-in `enter_room` event; there is no
   * server-side join handler to write. The phone joins a room named for its
   * token, the kiosk joins "kiosk".
   */
  function socketUrl() {
    /* Derive the scheme from the page. Hardcoding http:// breaks under TLS as
     * mixed content; hardcoding wss:// breaks without it. */
    return window.location.origin;
  }

  function createSocket(room, handlers) {
    if (typeof io === "undefined") {
      /* The vendored client failed to load. Every page must still work by
       * polling, so this is a warning rather than a fatal error. */
      if (window.console) { console.warn("socket.io client not loaded; falling back to polling"); }
      return null;
    }

    var sock = io(socketUrl(), {
      /* The brick mounts Socket.IO at /socket.io with socketio_path="", so the
       * client must use the default engine.io path. Stated explicitly because
       * getting it wrong produces a silent failure to connect. */
      path: "/socket.io",
      transports: ["websocket", "polling"],
      reconnection: true,
      reconnectionDelay: 500,
      reconnectionDelayMax: 4000,
      timeout: 8000,
    });

    sock.on("connect", function () {
      if (room) { sock.emit("enter_room", room); }
      if (handlers && handlers.onConnect) { handlers.onConnect(); }
    });
    sock.on("disconnect", function () {
      if (handlers && handlers.onDisconnect) { handlers.onDisconnect(); }
    });
    sock.on("connect_error", function () {
      if (handlers && handlers.onError) { handlers.onError(); }
    });

    var events = (handlers && handlers.events) || {};
    for (var name in events) {
      if (Object.prototype.hasOwnProperty.call(events, name)) {
        /* Closure per iteration is unnecessary here because `name` is only read
         * synchronously to register; the callback captures `events` and looks up
         * by its own bound name. */
        (function (evName) {
          sock.on(evName, function (data) { events[evName](data); });
        })(name);
      }
    }
    return sock;
  }

  /* ── Countdown ──────────────────────────────────────────────────────────
   * Rendered client-side from a deadline the server supplies, so there is no
   * per-second network traffic and a laggy connection does not make the timer
   * stutter.
   *
   * The deadline arrives as `deadline_mono`, which is the SERVER's monotonic
   * clock and is meaningless on the phone. So it is converted to a local deadline
   * using the offset between the two at the moment the payload arrived:
   *   local_deadline = Date.now() + (deadline_mono - server_now_mono)
   * The caller passes `remaining_ms`, which the server also provides, to avoid
   * needing the server's clock at all. That is the value used here.
   */
  function Countdown(onTick, onExpire) {
    this._onTick = onTick;
    this._onExpire = onExpire;
    this._endsAt = 0;
    this._timer = null;
    this._expired = false;
    /* Initialised here, not only in startWithTotal. `start()` on its own is a
     * legitimate call, and reading an undefined total made the progress fraction
     * collapse to left/1 — a bar that emptied in one millisecond. Defaulting to
     * the remaining time gives a fraction that starts at 1 and falls to 0, which
     * is the sensible reading of "I do not know the total". */
    this._totalMs = 1;
  }

  Countdown.prototype.start = function (remainingMs) {
    this.stop();
    this._expired = false;
    var ms = Math.max(0, Number(remainingMs) || 0);
    /* Only adopt the remaining time as the total when no explicit total was set,
     * so startWithTotal keeps its own denominator. */
    if (!this._explicitTotal) { this._totalMs = Math.max(1, ms); }
    this._endsAt = Date.now() + ms;
    var self = this;
    /* 50 ms is smooth enough to read as continuous and cheap enough to leave
     * running on a phone for a 14 second round. */
    this._timer = window.setInterval(function () { self._tick(); }, 50);
    this._tick();
  };

  Countdown.prototype._tick = function () {
    var left = this._endsAt - Date.now();
    if (left <= 0) {
      if (!this._expired) {
        this._expired = true;
        if (this._onTick) { this._onTick(0, 0); }
        this.stop();
        if (this._onExpire) { this._onExpire(); }
      }
      return;
    }
    if (this._onTick) {
      var total = this._totalMs > 0 ? this._totalMs : 1;
      this._onTick(left, Math.max(0, Math.min(1, left / total)));
    }
  };

  Countdown.prototype.startWithTotal = function (remainingMs, totalMs) {
    this._totalMs = Math.max(1, Number(totalMs) || 1);
    this._explicitTotal = true;
    this.start(remainingMs);
  };

  Countdown.prototype.stop = function () {
    if (this._timer !== null) {
      window.clearInterval(this._timer);
      this._timer = null;
    }
  };

  Countdown.prototype.remaining = function () {
    return Math.max(0, this._endsAt - Date.now());
  };

  /* ── Haptics ────────────────────────────────────────────────────────────
   * Feature-detected, because iOS Safari does not implement navigator.vibrate.
   * There is always a visual flash alongside it, so an iPhone user loses nothing.
   */
  function vibrate(pattern) {
    try {
      if (navigator.vibrate) { navigator.vibrate(pattern); }
    } catch (e) { /* ignored: haptics are a nicety, never a requirement */ }
  }

  /* ── QR ─────────────────────────────────────────────────────────────────
   * Generated client-side from the session URL. The 8x13 LED matrix can never
   * show a QR code (the smallest is 21x21 modules), so the laptop screen is the
   * only place it can appear.
   */
  function renderQr(container, text, sizePx) {
    if (!container) { return false; }
    clear(container);
    if (typeof QRCode === "undefined") { return false; }
    try {
      /* eslint-disable-next-line no-new */
      new QRCode(container, {
        text: text,
        width: sizePx || 220,
        height: sizePx || 220,
        /* Level M rather than H: the code is scanned off a laptop screen at arm's
         * length in a lit room, where M is ample, and a lower level means fewer
         * modules, which means a larger, easier-to-scan code at the same pixel
         * size. H would be a smaller, denser grid for no benefit. */
        correctLevel: QRCode.CorrectLevel.M,
      });
      return true;
    } catch (e) {
      return false;
    }
  }

  /* ── Storage ────────────────────────────────────────────────────────────
   * Wrapped because localStorage throws in private-browsing modes and when the
   * origin is opaque. A page that crashes because it could not remember an admin
   * token is a page that will not load at a demo.
   */
  var store = {
    get: function (key) {
      try { return window.localStorage.getItem(key); } catch (e) { return null; }
    },
    set: function (key, value) {
      try { window.localStorage.setItem(key, value); } catch (e) { /* ignored */ }
    },
    remove: function (key) {
      try { window.localStorage.removeItem(key); } catch (e) { /* ignored */ }
    },
  };

  window.PT = {
    t: t,
    errorMessage: errorMessage,
    strings: STRINGS,
    $: $,
    setText: setText,
    show: show,
    clear: clear,
    make: make,
    qs: qs,
    buildQuery: buildQuery,
    api: api,
    adminRequest: adminRequest,
    createSocket: createSocket,
    socketUrl: socketUrl,
    Countdown: Countdown,
    vibrate: vibrate,
    renderQr: renderQr,
    store: store,
  };
})();
