/*
 * PhishTap Lite — PIN join page logic.
 *
 * Short on purpose. Its only job is to turn a four-digit PIN into a session token
 * and hand off to game.html, which owns everything after that. Duplicating any
 * game logic here would give two places to keep in step.
 */

/* global PT */
(function () {
  "use strict";

  var el = {};
  var busy = false;

  function cacheElements() {
    var ids = ["title", "body", "pin", "pin-label", "pin-hint", "error", "btn-join", "foot"];
    for (var i = 0; i < ids.length; i++) { el[ids[i]] = PT.$(ids[i]); }
  }

  function applyStrings() {
    PT.setText(el.title, PT.t("join.title"));
    PT.setText(el.body, PT.t("join.body"));
    PT.setText(el["pin-label"], PT.t("join.pinLabel"));
    PT.setText(el["btn-join"], PT.t("join.submit"));
    PT.setText(el.foot, PT.t("brand.tagline"));
    el.pin.placeholder = "\u2022\u2022\u2022\u2022";
  }

  function showError(message) {
    PT.setText(el.error, message);
    PT.show(el.error, true);
  }

  function clearError() {
    PT.show(el.error, false);
  }

  function submit() {
    if (busy) { return; }

    /* Strip anything that is not a digit before sending. A paste from a messaging
     * app often carries spaces or a dash, and sending "12 34" would fail with a
     * confusing "wrong PIN" rather than being quietly corrected. */
    var pin = (el.pin.value || "").replace(/\D/g, "");
    if (pin.length !== 4) {
      showError(PT.t("join.noSession"));
      return;
    }

    busy = true;
    el["btn-join"].disabled = true;
    PT.setText(el["btn-join"], PT.t("join.scanning"));
    clearError();

    PT.api.join(pin).then(function (res) {
      busy = false;
      el["btn-join"].disabled = false;
      PT.setText(el["btn-join"], PT.t("join.submit"));

      if (!res || !res.ok || !res.token) {
        /* The server's message is preferred: it carries the remaining attempt
         * count, and after five failures it says the IP is locked out. Replacing
         * that with a generic string would leave the player retrying forever
         * against a lockout they cannot see. */
        showError((res && res.message) || PT.t("errors.E_PIN"));
        el.pin.select();
        return;
      }

      /* Hand off to the game. The token goes in the query string, which is how
       * every other entry point works, so game.html needs no idea how the player
       * arrived. */
      window.location.href = "game.html?token=" + encodeURIComponent(res.token);
    });
  }

  function boot() {
    cacheElements();
    applyStrings();

    el["btn-join"].addEventListener("click", submit);
    el.pin.addEventListener("keydown", function (e) {
      if (e.key === "Enter") { e.preventDefault(); submit(); }
    });
    /* Clear the error as soon as they start retyping. A stale error next to a
     * corrected PIN reads as "still wrong" and costs another submit to disprove. */
    el.pin.addEventListener("input", clearError);

    el.pin.focus();
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", boot);
  } else {
    boot();
  }
})();
