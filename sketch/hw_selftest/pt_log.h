/*
 * PhishTap Lite — hw_selftest
 *
 * Logging and result reporting over the Bridge-backed Monitor channel.
 */

#ifndef PT_LOG_H
#define PT_LOG_H

#include <Arduino.h>
#include <Arduino_RouterBridge.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* 192 bytes is enough for the longest line this sketch emits (a summary row
 * plus a detail string) while staying small enough to keep on the stack. */
#define PT_LOG_BUF 192

/*
 * Bring up the console.
 *
 * WHY Bridge.begin() before Monitor.begin(): the STM32U585 has no USB of its
 * own. Monitor is not a real UART — it is a virtual serial channel carried over
 * the internal UART to the Linux side by the arduino-router service, and
 * Monitor.begin() queries the router's mon/connected RPC method. Asking for the
 * monitor before the bridge is up therefore cannot succeed.
 *
 * WHY Monitor and not Serial: Serial is only an alias for Monitor on core
 * releases >= 0.55.0. Depending on Serial would silently produce no output on an
 * older core, which is exactly the failure mode a bring-up selftest cannot
 * afford.
 */
inline bool logInit(void) {
  Bridge.begin();
  return Monitor.begin();
}

inline void logRaw(const char* s) {
  Monitor.println(s);
}

inline void logBlank(void) {
  Monitor.println("");
}

/* printf-style logging. Only integer and string conversions are used anywhere
 * in this sketch; see logFloat() for why floating point is handled separately.
 *
 * Named ptLogf, not logf: logf is the single-precision logarithm from <math.h>.
 * Shadowing a C standard library name in the global namespace compiles today
 * (C++ treats the two as overloads) but is fragile across toolchains and
 * misleading to read, so it follows the project's pt prefix convention instead.
 */
inline void ptLogf(const char* fmt, ...) {
  char buf[PT_LOG_BUF];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Monitor.println(buf);
}

/*
 * Print a float without ever passing %f to printf.
 *
 * WHY: the Zephyr/ARM toolchain commonly links newlib-nano, whose snprintf omits
 * floating-point formatting unless the build passes -u _printf_float. That
 * failure is SILENT — %f expands to nothing rather than producing an error — so
 * a diagnostic that printed a timing figure would just show a hole. Splitting
 * the value into integer and fraction parts with integer arithmetic works on
 * every C library and costs a few instructions.
 */
inline void logFloat(const char* label, float value, unsigned char decimals,
                     const char* unit) {
  bool negative = (value < 0.0f);
  if (negative) value = -value;

  long scale = 1;
  for (unsigned char i = 0; i < decimals; ++i) scale *= 10;

  /* +0.5f rounds half-up; the value is already non-negative here. */
  long scaled = (long)(value * (float)scale + 0.5f);
  long whole  = scaled / scale;
  long frac   = scaled % scale;

  char buf[PT_LOG_BUF];
  snprintf(buf, sizeof(buf), "%s: %s%ld.%0*ld%s",
           label, negative ? "-" : "", whole,
           (int)decimals, frac, (unit != NULL) ? unit : "");
  Monitor.println(buf);
}

/* ── Verdicts ───────────────────────────────────────────────────────────────
 * PT_VISUAL is a first-class outcome, not a soft pass. The matrix and the LEDs
 * have no readback path, so the firmware genuinely cannot tell whether they
 * lit up. Reporting PASS for those would be a lie; reporting FAIL would be
 * wrong too. VISUAL means "a human must confirm this".
 *
 * PT_WARN is an ADVISORY: something is not textbook, but it does not stop the
 * project. This distinction matters. A counterfeit RC522 reports a version byte
 * the library has no reference table for, so PCD_PerformSelfTest() aborts and
 * returns false, and several analog receiver registers hold different defaults
 * than the NXP datasheet lists. None of that prevents reading a UID, which is
 * all this project does. Scoring those as hard failures produced a summary that
 * said "do not start Stage 1b" about a reader that had just read a card five
 * times out of five. Only PT_FAIL blocks.
 */
typedef enum {
  PT_PASS = 0,
  PT_FAIL,
  PT_WARN,
  PT_VISUAL,
  PT_SKIP
} PtVerdict;

inline const char* verdictName(PtVerdict v) {
  switch (v) {
    case PT_PASS:   return "PASS";
    case PT_FAIL:   return "FAIL";
    case PT_WARN:   return "WARN";
    case PT_VISUAL: return "VISUAL";
    case PT_SKIP:   return "SKIP";
  }
  return "?";
}

/* Sized for every row the full run can produce: A1-A3, L, W, B0-B5 plus the
 * Phase B bottom line, and C1-C3 is sixteen, plus headroom so a future phase
 * cannot silently drop results. Exceeding the cap does not crash - recordResult()
 * stops storing - but the summary would be incomplete, which is worse than using
 * a few more bytes. */
#define PT_MAX_RESULTS 20

typedef struct {
  const char* phase;
  PtVerdict   verdict;
  char        detail[72];
} PtResult;

static PtResult s_results[PT_MAX_RESULTS];
static unsigned char s_resultCount = 0;

/* Clear the table. Needed when the whole selftest is run more than once (see
 * PT_LOOP_FOREVER), because recordResult() stops storing once the fixed-size
 * table is full and a second pass would otherwise be silently dropped. */
inline void resetResults(void) {
  s_resultCount = 0;
}

/* Record a verdict and echo it immediately, so the operator sees the conclusion
 * right next to the evidence that produced it rather than only at the end. */
inline void recordResult(const char* phase, PtVerdict verdict, const char* detail) {
  char line[PT_LOG_BUF];
  bool hasDetail = (detail != NULL) && (detail[0] != '\0');
  snprintf(line, sizeof(line), ">>> [%s] %s%s%s",
           phase, verdictName(verdict), hasDetail ? " - " : "",
           hasDetail ? detail : "");
  Monitor.println(line);

  if (s_resultCount >= PT_MAX_RESULTS) return;
  PtResult* r = &s_results[s_resultCount++];
  r->phase   = phase;
  r->verdict = verdict;
  snprintf(r->detail, sizeof(r->detail), "%s", hasDetail ? detail : "");
}

inline void logSection(const char* title) {
  logBlank();
  Monitor.println("==============================================");
  Monitor.println(title);
  Monitor.println("==============================================");
}

inline void logStep(const char* text) {
  Monitor.println(text);
}

/*
 * Final report.
 *
 * Only PT_FAIL blocks. PT_WARN rows are advisories worth reading but not worth
 * stopping for, and PT_VISUAL rows mark the boundary of what firmware can verify
 * on its own. Conflating the three is how a working reader gets reported as a
 * failed bring-up.
 */
inline void printSummary(void) {
  logBlank();
  Monitor.println("================= SUMMARY ==================");
  for (unsigned char i = 0; i < s_resultCount; ++i) {
    char line[PT_LOG_BUF];
    snprintf(line, sizeof(line), "  %-13s %-7s %s",
             s_results[i].phase, verdictName(s_results[i].verdict),
             s_results[i].detail);
    Monitor.println(line);
  }
  Monitor.println("=============================================");

  unsigned char fails = 0, warns = 0, visuals = 0;
  for (unsigned char i = 0; i < s_resultCount; ++i) {
    switch (s_results[i].verdict) {
      case PT_FAIL:   ++fails;   break;
      case PT_WARN:   ++warns;   break;
      case PT_VISUAL: ++visuals; break;
      default: break;
    }
  }

  if (fails > 0) {
    ptLogf("%u BLOCKING failure(s). Resolve these before Stage 1b.",
           (unsigned)fails);
  } else {
    Monitor.println("No blocking failures.");
  }
  if (warns > 0) {
    ptLogf("%u advisory warning(s) - read them, but they do not block progress.",
           (unsigned)warns);
  }
  if (visuals > 0) {
    ptLogf("%u row(s) marked VISUAL need your eyes or ears - see the log above.",
           (unsigned)visuals);
  }
}

#endif /* PT_LOG_H */
