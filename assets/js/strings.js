/*
 * PhishTap Lite — every user-facing string, in one place.
 *
 * WHY THIS FILE EXISTS
 * docs/ARCHITECTURE.md §10 calls for a single strings file so the UI can be
 * translated. That is the stated reason, but the practical one is stronger: this
 * is a teaching tool about phishing, and the wording IS the product. "Is this
 * message trying to rush you?" and "Check the message" are the same instruction
 * to a compiler and completely different to a first-year student. Having every
 * word in one file means the wording can be reviewed and revised as a whole
 * instead of being discovered scattered across five pages.
 *
 * CONVENTIONS
 *   - Keys are grouped by page, then by purpose.
 *   - Anything the server can return as an E_* code has an entry in `errors`, so
 *     a code never reaches the screen raw.
 *   - Interpolation uses {name} placeholders and common.js's `t()` helper. Do
 *     not build sentences by concatenating translated fragments: word order
 *     differs between languages and concatenation cannot be translated.
 *   - British/Indian English spelling is used consistently ("recognise",
 *     "behaviour"), matching the rest of the project's prose.
 *
 * This file defines a single global, `PT_STRINGS`, and nothing else.
 */

/* global PT_STRINGS */
window.PT_STRINGS = {

  meta: {
    /* Bumped whenever the consent wording changes materially. The server stores
     * the version a player agreed to, so changing this text re-prompts everyone
     * rather than silently applying new terms to old agreements. */
    consentVersion: 1,
  },

  brand: {
    name: "PhishTap",
    tagline: "Spot the fake. Beat the clock.",
  },

  /* ── Kiosk (the laptop screen everyone watches) ───────────────────────── */
  kiosk: {
    attractTitle: "PhishTap",
    attractBody: "Tap a card on the reader to play.",
    attractHint: "Can you spot the phishing message before the timer runs out?",

    waitingForPhone: "Waiting for a phone to join",
    scanToJoin: "Scan to join",
    orEnterPin: "or enter the PIN on your phone",
    pinLabel: "PIN",
    sessionExpires: "This code expires in {seconds}s",

    playerJoined: "{nickname} joined",
    roundLabel: "Round",
    livesLabel: "Lives",
    scoreLabel: "Score",
    streakLabel: "Streak",

    leaderboardTitle: "High scores",
    leaderboardEmpty: "No scores yet. Be the first.",
    leaderboardRank: "#",

    gameOverTitle: "Game over",
    finalScore: "{nickname} scored {score}",
    roundsPlayed: "{count} rounds played",
    reasonNoLives: "Out of lives",
    reasonPoolExhausted: "All messages cleared",
    reviewHint: "The phone now shows what was wrong in every message.",

    /* Fault banners. These are deliberately blunt: a demo where nobody can tell
     * whether the console is broken is a demo that gets abandoned. */
    hwFault: "Hardware fault: {code}",
    linkLost: "Console not responding. Check the board is powered and the app is running.",
    linkRestored: "Console reconnected.",
    noSession: "No active session",

    footerBaseUrl: "Board address",
  },

  /* ── Game (the phone) ─────────────────────────────────────────────────── */
  game: {
    /* Consent screen. Shown once per player per card. */
    consentTitle: "Before you play",
    consentBody:
      "PhishTap records your answers so it can show you which phishing tricks "
      + "you missed, and so a leaderboard can compare scores.",
    consentPseudonymous:
      "You are identified by a code derived from the card you tapped and the "
      + "nickname you choose. The card's raw serial number is never stored and "
      + "never leaves the console. This is pseudonymisation, not anonymity: the "
      + "same card and nickname always map back to the same record.",
    consentRetention:
      "Records stay on this console. You can delete yours at any time from the "
      + "review screen at the end of the game, and an administrator can delete "
      + "any record.",
    consentNoRealData:
      "Every message in this game is fictional. No real company, bank or "
      + "university sent any of them, and none of the links work.",
    nicknameLabel: "Choose a nickname",
    nicknameHint: "3 to 12 characters. Shown on the leaderboard.",
    consentAgree: "I understand, let me play",
    consentDecline: "No thanks",
    consentDeclined: "No problem. Nothing was saved. Close this tab to leave.",

    /* Round play. */
    questionPrompt: "Is this message genuine?",
    buttonPhish: "PHISH",
    buttonSafe: "SAFE",
    buttonPhishHint: "It is trying to trick me",
    buttonSafeHint: "It is legitimate",

    channelEmail: "Email",
    channelSms: "SMS",
    channelWhatsapp: "WhatsApp",
    channelPoster: "Notice",
    subjectPrefix: "Subject",
    forwardedPrefix: "Forwarded message",

    timeLeft: "{seconds}s left",
    timeUp: "Time up",

    /* Feedback. Deliberately says nothing about WHY, because the explanation is
     * the teaching content and revealing it mid-game would let a player learn
     * the answers instead of learning to spot the cues. */
    correct: "Correct",
    wrong: "Wrong",
    wrongLostLife: "Wrong — you lost a life",
    timeoutLostLife: "Too slow — you lost a life",
    pointsEarned: "+{points}",
    streakBonus: "{streak} in a row",

    /* End of game. */
    gameOverTitle: "Game over",
    gameOverScore: "You scored {score}",
    gameOverRounds: "{count} rounds",
    seeReview: "See what you missed",
    playAgainHint: "Tap the card again to play another round.",

    /* Disconnection. The server still grades the round by its own deadline, so
     * these messages must not imply the round was cancelled. */
    reconnecting: "Connection lost. Reconnecting…",
    reconnected: "Reconnected.",
    sessionGone: "This session has ended. Tap the card on the console to start a new one.",
  },

  /* ── Review (the phone, after the game) ───────────────────────────────── */
  review: {
    title: "What you missed",
    intro:
      "Every message you saw, with the trick it used. This is the part that "
      + "actually teaches you something.",
    scoreSummary: "Score {score} across {count} rounds",

    youSaid: "You said",
    itWas: "It was",
    labelPhish: "Phishing",
    labelSafe: "Genuine",
    youWereRight: "You were right",
    youWereWrong: "You were wrong",
    youTimedOut: "You ran out of time",
    reactionTime: "Answered in {ms} ms",

    cuesTitle: "The tells",
    noCues: "Nothing suspicious — that was the point.",
    explanationTitle: "Why",
    lessonTitle: "Remember",

    cueNames: {
      urgency: "Manufactured urgency",
      sender_mismatch: "Sender does not match the claim",
      lookalike_domain: "Lookalike domain",
      generic_greeting: "Generic greeting",
      suspicious_link: "Suspicious link",
      credential_request: "Asks for a password or OTP",
      unexpected_attachment: "Unexpected attachment",
      too_good: "Too good to be true",
      authority_claim: "Claims authority it cannot prove",
      payment_request: "Asks you to pay first",
      emotional_appeal: "Emotional pressure",
      vague_details: "Vague on specifics",
    },

    deleteTitle: "Your data",
    deleteBody:
      "Delete every record tied to this session. This cannot be undone, and it "
      + "will remove you from the leaderboard.",
    deleteButton: "Delete my data",
    deleteConfirm: "Really delete? This cannot be undone.",
    deleteDone: "Deleted. Your nickname has been removed from the leaderboard.",
    deleteFailed: "Could not delete. Tell the person running this console.",

    expired: "This review link has expired. It stays valid for ten minutes after the game.",
    backToKiosk: "Back to the console screen",
  },

  /* ── Join (PIN fallback) ──────────────────────────────────────────────── */
  join: {
    title: "Enter the PIN",
    body: "The QR code did not work? Type the four-digit PIN shown on the console.",
    pinLabel: "PIN",
    submit: "Join",
    scanning: "Joining…",
    noSession: "There is no active game right now. Tap a card on the console first.",
  },

  /* ── Admin ────────────────────────────────────────────────────────────── */
  admin: {
    title: "PhishTap admin",
    loginTitle: "Sign in",
    loginBody: "The PIN was printed to the Python console when the app started.",
    pinLabel: "Admin PIN",
    login: "Sign in",
    logout: "Sign out",

    sectionOverview: "Overview",
    sectionAnalytics: "Analytics",
    sectionPlayers: "Players",
    sectionDanger: "Danger zone",

    healthTitle: "System",
    healthBridge: "Console link",
    healthUp: "up",
    healthDown: "DOWN",
    healthVersion: "Firmware",
    healthUptime: "Backend uptime",
    healthBaseUrl: "Board address",

    cohortTitle: "Cohort accuracy",
    cohortTotal: "Answers recorded",
    cohortAccuracy: "Accuracy",
    cohortTimeouts: "Timeouts",
    cohortPhish: "Phishing items",
    cohortSafe: "Genuine items",
    noData: "No data yet.",

    cueTitle: "Accuracy by cue",
    cueHeader: ["Cue", "Seen", "Correct", "Accuracy", "Timeouts"],
    cueHint:
      "Low accuracy on a cue means students do not recognise that trick. That is "
      + "the cue to spend more time on.",

    reactionTitle: "Reaction time",
    reactionHint:
      "Percentiles, not an average: a few students read for the full limit, which "
      + "drags a mean away from what anyone actually did. Phone and button timings "
      + "are kept apart because they measure different things.",
    reactionSource: "Source",
    reactionN: "n",
    reactionMin: "min",
    reactionP50: "p50",
    reactionP90: "p90",
    reactionMax: "max",
    reactionExcluded: "{count} answers excluded (no render acknowledgement)",

    funnelTitle: "Funnel",
    funnelTaps: "Card taps",
    funnelCreated: "Sessions created",
    funnelStarted: "Players identified",
    funnelCompleted: "Games completed",
    funnelExpired: "Sessions expired",
    funnelHint:
      "A gap between taps and sessions created means phones could not reach the "
      + "board. A gap between created and identified means people bailed at the "
      + "consent screen. Those are different problems with different fixes.",

    exportButton: "Download anonymised CSV",
    exportHint:
      "Pseudonymous player codes and measurements only. No nicknames, no card "
      + "references, no IP addresses.",

    playerDeleteLabel: "Delete a player by code",
    playerDeletePlaceholder: "player_id",
    playerDeleteButton: "Delete",

    clockTitle: "Clock offset",
    clockBody:
      "This board has no real-time clock, so recorded timestamps can be wrong. "
      + "Set the offset in milliseconds to correct them. Durations are unaffected "
      + "either way, because they are measured monotonically.",
    clockLabel: "Offset (ms)",
    clockSet: "Apply",
    clockCurrent: "Current offset",

    resetTitle: "Erase everything",
    resetBody:
      "Deletes every player, session and answer. Type RESET to confirm. This "
      + "cannot be undone and there is no backup except the CSV export above.",
    resetLabel: "Type RESET",
    resetButton: "Erase all data",

    saved: "Done.",
  },

  /* ── Errors ─────────────────────────────────────────────────────────────
   * Every E_* code the server can return, from docs/ARCHITECTURE.md §4. A code
   * with no entry falls back to `generic`, so a new server-side code degrades to
   * an understandable message instead of showing "E_WHATEVER" to a student.
   */
  errors: {
    E_RFID_INIT: "The card reader did not start. Tell the person running this console.",
    E_RFID_READ: "The card reader is not reading. Try holding the card flat against it.",
    E_MATRIX: "The display failed to start.",
    E_BRIDGE: "The console stopped responding.",
    E_BUSY: "Someone is already playing. Wait for them to finish.",
    E_COOLDOWN: "Too soon. Wait a moment and tap again.",
    E_TOKEN: "That link is not valid.",
    E_EXPIRED: "That session has expired. Tap the card again to start a new one.",
    E_PIN: "That PIN is not correct.",
    E_RATE: "Too many attempts. Wait a moment and try again.",
    E_NICKNAME: "That nickname is not allowed. Try 3 to 12 characters.",
    E_CONSENT: "Consent has already been given for this session.",
    E_DB: "The console could not save that. Try again.",
    E_CORPUS: "The message library failed to load.",
    E_INTERNAL: "Something went wrong on the console.",
    generic: "Something went wrong. Try again.",
    network: "Cannot reach the console. Check you are on the same Wi-Fi network.",
  },

  /* ── Shared fragments ─────────────────────────────────────────────────── */
  common: {
    loading: "Loading…",
    retry: "Try again",
    close: "Close",
    cancel: "Cancel",
    confirm: "Confirm",
    yes: "Yes",
    no: "No",
    none: "—",
    secondsShort: "s",
    millisecondsShort: "ms",
  },
};
