/*
 * PhishTap Lite — review page logic.
 *
 * This page is the pedagogical payload of the whole project. Everything before it
 * exists to get a student to this screen: the game is the hook, this is the lesson.
 * So the ordering of information here is a design decision, not a layout accident.
 *
 * Each round shows, in order:
 *   1. what the player answered and whether it was right
 *   2. the message again, in full
 *   3. the cues that gave it away, as named chips
 *   4. why those cues matter
 *   5. the transferable lesson
 *
 * The verdict comes FIRST, before the message. A student who has to re-read the
 * whole message to find out whether they were right loses the thread; telling them
 * immediately lets them read the message again with the answer in hand, which is
 * the only way the explanation actually lands.
 *
 * Nothing on this page is trusted. Every field goes through setText, including
 * sender names, subjects and bodies, all of which are attacker-shaped by design.
 */

/* global PT */
(function () {
  "use strict";

  var LABEL_PHISH = 1;   /* must match python/phishtap/corpus.py */
  var LABEL_SAFE = 0;

  var el = {};
  var reviewToken = "";
  var deleting = false;

  function cacheElements() {
    var ids = [
      "screen-loading", "loading-title", "loading-text",
      "screen-expired", "expired-title", "expired-text",
      "screen-review", "review-title", "review-score", "review-summary", "review-intro",
      "rounds", "data-title", "data-body", "data-id", "data-error", "data-done", "btn-delete",
    ];
    for (var i = 0; i < ids.length; i++) { el[ids[i]] = PT.$(ids[i]); }
  }

  function applyStrings() {
    PT.setText(el["loading-title"], PT.t("brand.name"));
    PT.setText(el["loading-text"], PT.t("common.loading"));
    PT.setText(el["expired-title"], PT.t("brand.name"));
    PT.setText(el["expired-text"], PT.t("review.expired"));
    PT.setText(el["review-title"], PT.t("review.title"));
    PT.setText(el["review-intro"], PT.t("review.intro"));
    PT.setText(el["data-title"], PT.t("review.deleteTitle"));
    PT.setText(el["data-body"], PT.t("review.deleteBody"));
    PT.setText(el["btn-delete"], PT.t("review.deleteButton"));
  }

  function labelName(label) {
    if (label === LABEL_PHISH) { return PT.t("review.labelPhish"); }
    if (label === LABEL_SAFE) { return PT.t("review.labelSafe"); }
    return PT.t("common.none");
  }

  function labelClass(label) {
    if (label === LABEL_PHISH) { return "was-phish"; }
    if (label === LABEL_SAFE) { return "was-safe"; }
    return "muted";
  }

  /* A cue key becomes a human phrase via strings.js. An unknown cue falls back to
   * the raw key rather than disappearing: a new cue added to corpus.json without a
   * translation should still show up, visibly unpolished, so someone notices and
   * adds the string. Silently dropping it would hide content from the lesson. */
  function cueName(cue) {
    var table = (PT.strings.review && PT.strings.review.cueNames) || {};
    return table[cue] || String(cue);
  }

  function buildRound(r) {
    var card = PT.make("article", "round " + (r.you_were_right ? "right" : "wrong"));

    /* ── Header: round number and the verdict ───────────────────────────── */
    var top = PT.make("div", "round-top");
    top.appendChild(PT.make("span", "round-n",
      PT.t("kiosk.roundLabel") + " " + ((Number(r.round) || 0) + 1)));

    var verdictPill = PT.make("span", "pill " + (r.you_were_right ? "pill-safe" : "pill-phish"));
    PT.setText(verdictPill, r.you_were_right ? PT.t("review.youWereRight")
                                             : (r.timed_out ? PT.t("review.youTimedOut")
                                                            : PT.t("review.youWereWrong")));
    top.appendChild(verdictPill);
    card.appendChild(top);

    /* ── Verdict lines, before the message ──────────────────────────────── */
    var verdict = PT.make("div", "round-verdict");

    var youSaid = PT.make("div", "verdict-line");
    youSaid.appendChild(PT.make("span", "muted", PT.t("review.youSaid") + ": "));
    var youSaidVal = PT.make("b", null);
    /* A timeout has no answer. Showing "Genuine" for a null answer would be a
     * lie, and a lie on the page that is supposed to teach accuracy is worse than
     * an awkward dash. */
    if (r.your_answer === LABEL_PHISH || r.your_answer === LABEL_SAFE) {
      youSaidVal.className = labelClass(r.your_answer);
      PT.setText(youSaidVal, labelName(r.your_answer));
    } else {
      youSaidVal.className = "muted";
      PT.setText(youSaidVal, PT.t("review.youTimedOut"));
    }
    youSaid.appendChild(youSaidVal);
    verdict.appendChild(youSaid);

    var itWas = PT.make("div", "verdict-line");
    itWas.appendChild(PT.make("span", "muted", PT.t("review.itWas") + ": "));
    var itWasVal = PT.make("b", labelClass(r.correct_label), labelName(r.correct_label));
    itWas.appendChild(itWasVal);
    verdict.appendChild(itWas);

    if (r.points) {
      verdict.appendChild(PT.make("div", "verdict-line muted",
        PT.t("game.pointsEarned", { points: r.points })));
    }
    if (r.reaction_ms) {
      verdict.appendChild(PT.make("div", "verdict-line muted",
        PT.t("review.reactionTime", { ms: r.reaction_ms })));
    }
    card.appendChild(verdict);

    /* ── The message again ──────────────────────────────────────────────── */
    var msg = PT.make("div", "round-msg");
    msg.appendChild(PT.make("div", "round-sender", r.sender || ""));
    if (r.subject) {
      msg.appendChild(PT.make("div", "round-subject",
        PT.t("game.subjectPrefix") + ": " + r.subject));
    }
    /* setText. The body contains lookalike domains and defanged URLs on purpose;
     * innerHTML here would be a stored XSS vector pointing at a page every student
     * in the room is told to trust. */
    msg.appendChild(PT.make("div", "round-body", r.body || ""));
    card.appendChild(msg);

    /* ── The teaching part ──────────────────────────────────────────────── */
    var teach = PT.make("div", "round-teach");

    teach.appendChild(PT.make("div", "teach-h", PT.t("review.cuesTitle")));
    var cues = PT.make("div", "cues");
    var cueList = r.cues || [];
    if (!cueList.length) {
      /* A genuine message has no tells, and saying so is the lesson: not every
       * message is a scam, and expecting one trains people to ignore real notices. */
      cues.appendChild(PT.make("span", "cue", PT.t("review.noCues")));
    } else {
      for (var i = 0; i < cueList.length; i++) {
        cues.appendChild(PT.make("span", "cue", cueName(cueList[i])));
      }
    }
    teach.appendChild(cues);

    if (r.explanation) {
      teach.appendChild(PT.make("div", "teach-h", PT.t("review.explanationTitle")));
      teach.appendChild(PT.make("p", "teach-p", r.explanation));
    }
    if (r.lesson) {
      var lesson = PT.make("div", "lesson");
      lesson.appendChild(PT.make("div", "teach-h", PT.t("review.lessonTitle")));
      lesson.appendChild(PT.make("div", null, r.lesson));
      teach.appendChild(lesson);
    }

    card.appendChild(teach);
    return card;
  }

  function render(data) {
    PT.setText(el["review-score"], String(data.score || 0));
    PT.setText(el["review-summary"], PT.t("review.scoreSummary", {
      score: data.score || 0,
      count: data.rounds_played || 0,
    }));

    PT.clear(el.rounds);
    var rounds = data.rounds || [];
    for (var i = 0; i < rounds.length; i++) {
      el.rounds.appendChild(buildRound(rounds[i]));
    }

    /* The player code is shown so a student can ask an administrator to delete
     * their data later, after the review token has expired. It is a pseudonym, not
     * an identity, and showing it is what makes the deletion right exercisable
     * rather than theoretical. */
    PT.setText(el["data-id"], data.player_id ? ("player code: " + data.player_id) : "");

    PT.show(el["screen-loading"], false);
    PT.show(el["screen-expired"], false);
    PT.show(el["screen-review"], true);
  }

  function showExpired(message) {
    PT.setText(el["expired-text"], message || PT.t("review.expired"));
    PT.show(el["screen-loading"], false);
    PT.show(el["screen-review"], false);
    PT.show(el["screen-expired"], true);
  }

  /* ── Delete my data ─────────────────────────────────────────────────────
   * Scoped to the review token, NOT to a card tap. In shared-card mode several
   * students use one card, so a tap cannot identify who is asking and a
   * tap-scoped delete would let one student erase another's record. The review
   * token names exactly one player and only exists for ten minutes.
   */
  function onDelete() {
    if (deleting) { return; }
    /* A confirm dialog, because this is irreversible and the button sits at the
     * bottom of a long page where a thumb can land on it by accident while
     * scrolling. */
    if (!window.confirm(PT.t("review.deleteConfirm"))) { return; }

    deleting = true;
    el["btn-delete"].disabled = true;
    PT.show(el["data-error"], false);
    PT.show(el["data-done"], false);

    PT.api.deleteRequest(reviewToken).then(function (res) {
      deleting = false;
      if (!res || !res.ok) {
        el["btn-delete"].disabled = false;
        PT.setText(el["data-error"], (res && res.message) || PT.t("review.deleteFailed"));
        PT.show(el["data-error"], true);
        return;
      }
      PT.setText(el["data-done"], PT.t("review.deleteDone"));
      PT.show(el["data-done"], true);
      /* Hide the button rather than leave it enabled: a second delete would fail
       * confusingly, and the record is already gone. */
      PT.show(el["btn-delete"], false);
      PT.setText(el["data-id"], "");
    });
  }

  function boot() {
    cacheElements();
    applyStrings();

    reviewToken = PT.qs("token");
    if (!reviewToken) {
      showExpired(PT.t("review.expired"));
      return;
    }

    el["btn-delete"].addEventListener("click", onDelete);

    PT.api.review(reviewToken).then(function (res) {
      if (!res || !res.ok) {
        showExpired(res && res.message ? res.message : PT.t("review.expired"));
        return;
      }
      render(res);
    });
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", boot);
  } else {
    boot();
  }
})();
