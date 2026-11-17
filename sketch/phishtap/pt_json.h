/*
 * PhishTap Lite — production firmware (Stage 1b)
 *
 * Minimal JSON writer and reader over fixed char buffers.
 *
 * WHY THIS EXISTS
 * The bridge protocol (docs/ARCHITECTURE.md §4) carries every payload as a
 * single JSON string in both directions. That was chosen because MsgPack's
 * str_t IS arduino::String on this core (verified in
 * MsgPack/Types.h: `using str_t = String;` under `#ifdef ARDUINO`), so a
 * `String handler(String)` RPC binds natively and no composite type mapping has
 * to be guessed at.
 *
 * The board has no ArduinoJson installed, and adding it would mean heap
 * allocation on every call plus a dependency this project does not otherwise
 * need. The payloads here are tiny and their shapes are fixed and known at both
 * ends, so a small purpose-built codec is both lighter and easier to reason
 * about.
 *
 * WHY IT IS PURE C++ WITH NO Arduino.h DEPENDENCY
 * Deliberate. This file includes only <stddef.h>, <stdint.h>, <string.h> and
 * <stdio.h>, all of which exist identically on the host and on the Zephyr
 * toolchain. That means it compiles and runs under plain g++ on a laptop, so
 * tools/test_pt_json.cpp can exercise every branch with real assertions BEFORE
 * the code is ever flashed to hardware. Parsing bugs are the kind that fail
 * silently and confusingly on a board; this design makes them fail loudly on a
 * laptop instead.
 *
 * SCOPE LIMITATION, STATED PLAINLY
 * The reader only matches keys in the TOP-LEVEL object. Nested objects and
 * arrays are skipped wholesale, so a key buried inside a nested value can never
 * be returned by mistake. Every inbound payload in this protocol is flat —
 * {}, {since:N}, {seq:N}, {state:"x"}, {round:N,limit_ms:N}, {result:"x"},
 * {n:N}, {score:N}, {pattern:"x"} — so top-level-only is sufficient. Outbound
 * payloads DO nest (pt.get_events returns an array of objects), but those are
 * written by this module, never read by it.
 *
 * This is not a general-purpose JSON library and must not be used as one.
 */

#ifndef PT_JSON_H
#define PT_JSON_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>   /* strtol, strtoul */
#include <string.h>
#include <stdio.h>    /* snprintf - integer formats only, see ptJsonInt */

/* ── Writer ───────────────────────────────────────────────────────────────
 * Appends into a caller-supplied buffer. Every append is bounds-checked and,
 * once the buffer is full, the writer latches an overflow flag and stops rather
 * than writing past the end. A truncated response that is detected is a bug the
 * caller can report; a buffer overrun is a crash.
 */

#define PT_JSON_MAX_KEY 24   /* longest key name this protocol uses is "uptime_ms" */

typedef struct {
  char*  buf;
  size_t cap;
  size_t len;
  bool   needComma;   /* next element in the current object/array needs a ',' */
  bool   overflow;    /* latched: something did not fit */
} PtJsonWriter;

inline void ptJsonInit(PtJsonWriter* w, char* buf, size_t cap) {
  w->buf       = buf;
  w->cap       = cap;
  w->len       = 0;
  w->needComma = false;
  w->overflow  = false;
  if (buf != NULL && cap > 0) buf[0] = '\0';
}

/* Append one character. Returns false if it did not fit. */
inline bool ptJsonPutc(PtJsonWriter* w, char c) {
  if (w->overflow || w->buf == NULL) return false;
  /* Reserve one byte for the terminating NUL at all times. */
  if (w->len + 1 >= w->cap) { w->overflow = true; return false; }
  w->buf[w->len++] = c;
  w->buf[w->len]   = '\0';
  return true;
}

/* Append a NUL-terminated string verbatim (no escaping). Internal use: the
 * callers pass only literals and already-escaped content. */
inline bool ptJsonPuts(PtJsonWriter* w, const char* s) {
  if (s == NULL) return false;
  bool ok = true;
  for (const char* p = s; *p != '\0'; ++p) {
    if (!ptJsonPutc(w, *p)) { ok = false; break; }
  }
  return ok;
}

/* Emit the separator before a value or key. */
inline void ptJsonSep(PtJsonWriter* w) {
  if (w->needComma) ptJsonPutc(w, ',');
  w->needComma = true;
}

/* Append a string as a properly escaped JSON string literal.
 *
 * Escaping is not optional here: uid_hex and state names are safe ASCII today,
 * but a nickname or an error string could contain a quote, and an unescaped
 * quote would corrupt the whole payload and make Python's json.loads throw.
 * Control characters are escaped as \u00XX, which is what the JSON spec
 * requires — a raw control byte in a JSON string is invalid.
 */
inline bool ptJsonStr(PtJsonWriter* w, const char* s) {
  ptJsonSep(w);
  if (!ptJsonPutc(w, '"')) return false;
  if (s != NULL) {
    for (const unsigned char* p = (const unsigned char*)s; *p != '\0'; ++p) {
      unsigned char c = *p;
      bool ok = true;
      switch (c) {
        case '"':  ok = ptJsonPuts(w, "\\\""); break;
        case '\\': ok = ptJsonPuts(w, "\\\\"); break;
        case '\b': ok = ptJsonPuts(w, "\\b");  break;
        case '\f': ok = ptJsonPuts(w, "\\f");  break;
        case '\n': ok = ptJsonPuts(w, "\\n");  break;
        case '\r': ok = ptJsonPuts(w, "\\r");  break;
        case '\t': ok = ptJsonPuts(w, "\\t");  break;
        default:
          if (c < 0x20) {
            char esc[8];
            /* Hand-built rather than snprintf("%04x") so the output is exact
             * regardless of any toolchain printf quirks. */
            static const char hexd[] = "0123456789abcdef";
            esc[0] = '\\'; esc[1] = 'u'; esc[2] = '0'; esc[3] = '0';
            esc[4] = hexd[(c >> 4) & 0x0F];
            esc[5] = hexd[c & 0x0F];
            esc[6] = '\0';
            ok = ptJsonPuts(w, esc);
          } else {
            ok = ptJsonPutc(w, (char)c);
          }
      }
      if (!ok) return false;
    }
  }
  return ptJsonPutc(w, '"');
}

/* Numbers go through snprintf. Integer conversions are always present even in
 * newlib-nano, which omits only the floating-point formats — the same finding
 * recorded in hw_selftest/pt_log.h. No floats are ever emitted here. */
inline bool ptJsonInt(PtJsonWriter* w, long v) {
  ptJsonSep(w);
  char tmp[24];
  snprintf(tmp, sizeof(tmp), "%ld", v);
  return ptJsonPuts(w, tmp);
}

inline bool ptJsonUint(PtJsonWriter* w, unsigned long v) {
  ptJsonSep(w);
  char tmp[24];
  snprintf(tmp, sizeof(tmp), "%lu", v);
  return ptJsonPuts(w, tmp);
}

inline bool ptJsonBool(PtJsonWriter* w, bool v) {
  ptJsonSep(w);
  return ptJsonPuts(w, v ? "true" : "false");
}

inline bool ptJsonNull(PtJsonWriter* w) {
  ptJsonSep(w);
  return ptJsonPuts(w, "null");
}

/* ── Containers ───────────────────────────────────────────────────────────
 * Nesting is tracked only by the caller's Begin/End pairing; the writer itself
 * keeps a single needComma flag, which is reset on every container open and
 * restored on close. That is enough for the shallow shapes this protocol uses
 * and avoids carrying a depth stack.
 */

inline bool ptJsonObjBegin(PtJsonWriter* w) {
  ptJsonSep(w);
  w->needComma = false;
  return ptJsonPutc(w, '{');
}

inline bool ptJsonObjEnd(PtJsonWriter* w) {
  w->needComma = true;   /* the closed object is itself a complete element */
  return ptJsonPutc(w, '}');
}

inline bool ptJsonArrBegin(PtJsonWriter* w) {
  ptJsonSep(w);
  w->needComma = false;
  return ptJsonPutc(w, '[');
}

inline bool ptJsonArrEnd(PtJsonWriter* w) {
  w->needComma = true;
  return ptJsonPutc(w, ']');
}

/* ── Key/value conveniences ─────────────────────────────────────────────── */

inline bool ptJsonKey(PtJsonWriter* w, const char* key) {
  ptJsonSep(w);
  if (!ptJsonPutc(w, '"')) return false;
  if (!ptJsonPuts(w, key)) return false;
  if (!ptJsonPuts(w, "\":")) return false;
  /* A value follows immediately, so it must not emit its own separator. */
  w->needComma = false;
  return true;
}

inline bool ptJsonKVStr(PtJsonWriter* w, const char* k, const char* v) {
  return ptJsonKey(w, k) && ptJsonStr(w, v);
}
inline bool ptJsonKVInt(PtJsonWriter* w, const char* k, long v) {
  return ptJsonKey(w, k) && ptJsonInt(w, v);
}
inline bool ptJsonKVUint(PtJsonWriter* w, const char* k, unsigned long v) {
  return ptJsonKey(w, k) && ptJsonUint(w, v);
}
inline bool ptJsonKVBool(PtJsonWriter* w, const char* k, bool v) {
  return ptJsonKey(w, k) && ptJsonBool(w, v);
}

/* Close out and report the length, or 0 if anything overflowed.
 *
 * Returning 0 on overflow is deliberate: a silently truncated JSON document is
 * worse than no document, because Python would raise a decode error with no
 * indication that the MCU ran out of buffer. The caller turns 0 into an explicit
 * error response instead.
 */
inline size_t ptJsonFinish(PtJsonWriter* w) {
  if (w->overflow || w->buf == NULL) {
    if (w->buf != NULL && w->cap > 0) w->buf[0] = '\0';
    return 0;
  }
  if (w->len < w->cap) w->buf[w->len] = '\0';
  return w->len;
}

/* ── Reader ─────────────────────────────────────────────────────────────── */

inline const char* ptJsonSkipWs(const char* p) {
  if (p == NULL) return NULL;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
  return p;
}

/* Parse a JSON string literal. p must point at the opening quote.
 * Returns the position just past the closing quote, or NULL if malformed.
 * Decoded characters are written to out (bounded by outCap, always NUL-ended).
 * out may be NULL, in which case the string is validated and skipped only.
 */
inline const char* ptJsonReadString(const char* p, char* out, size_t outCap) {
  if (p == NULL || *p != '"') return NULL;
  ++p;
  size_t n = 0;
  for (;;) {
    char c = *p;
    if (c == '\0') return NULL;          /* unterminated */
    if (c == '"') { ++p; break; }
    if (c == '\\') {
      ++p;
      char e = *p;
      if (e == '\0') return NULL;
      ++p;
      char dec;
      switch (e) {
        case '"':  dec = '"';  break;
        case '\\': dec = '\\'; break;
        case '/':  dec = '/';  break;
        case 'b':  dec = '\b'; break;
        case 'f':  dec = '\f'; break;
        case 'n':  dec = '\n'; break;
        case 'r':  dec = '\r'; break;
        case 't':  dec = '\t'; break;
        case 'u': {
          /* \uXXXX. This protocol never emits one, but a malformed or hostile
           * payload could contain one, so decode it rather than desync. Only
           * the ASCII range is representable here; anything above it becomes
           * '?', which is safe and visible. */
          unsigned code = 0;
          for (int i = 0; i < 4; ++i) {
            char h = *p;
            if (h == '\0') return NULL;
            ++p;
            unsigned d;
            if (h >= '0' && h <= '9')      d = (unsigned)(h - '0');
            else if (h >= 'a' && h <= 'f') d = (unsigned)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') d = (unsigned)(h - 'A' + 10);
            else return NULL;
            code = (code << 4) | d;
          }
          dec = (code > 0 && code < 0x80) ? (char)code : '?';
          break;
        }
        default: return NULL;             /* invalid escape */
      }
      if (out != NULL && n + 1 < outCap) out[n++] = dec;
    } else {
      if (out != NULL && n + 1 < outCap) out[n++] = c;
      ++p;
    }
  }
  if (out != NULL && outCap > 0) out[n] = '\0';
  return p;
}

/* Skip any JSON value. Returns the position past it, or NULL if malformed.
 * This is what keeps the reader honest about nesting: a non-matching member's
 * value is skipped in its entirety, so the scan can never descend into a nested
 * object and match a key there.
 */
inline const char* ptJsonSkipValue(const char* p) {
  p = ptJsonSkipWs(p);
  if (p == NULL || *p == '\0') return NULL;
  switch (*p) {
    case '"':
      return ptJsonReadString(p, NULL, 0);
    case '{':
    case '[': {
      char open  = *p;
      char close = (open == '{') ? '}' : ']';
      int depth = 0;
      while (*p != '\0') {
        char c = *p;
        if (c == '"') {
          p = ptJsonReadString(p, NULL, 0);
          if (p == NULL) return NULL;
          continue;
        }
        if (c == open)  ++depth;
        if (c == close) { --depth; if (depth == 0) return p + 1; }
        ++p;
      }
      return NULL;                        /* unbalanced */
    }
    case 't':
      return (strncmp(p, "true", 4) == 0)  ? p + 4 : NULL;
    case 'f':
      return (strncmp(p, "false", 5) == 0) ? p + 5 : NULL;
    case 'n':
      return (strncmp(p, "null", 4) == 0)  ? p + 4 : NULL;
    default: {
      /* number: [-+0-9.eE] run */
      const char* s = p;
      while (*p != '\0' &&
             ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' ||
              *p == '.' || *p == 'e' || *p == 'E')) ++p;
      return (p > s) ? p : NULL;
    }
  }
}

/* Locate a top-level key. On success returns a pointer to the first character
 * of its VALUE (whitespace already skipped), else NULL.
 *
 * keyBuf bounds the key copy so an absurdly long key in a malformed payload
 * cannot overflow the stack.
 */
inline const char* ptJsonFind(const char* json, const char* key) {
  if (json == NULL || key == NULL) return NULL;
  const char* p = ptJsonSkipWs(json);
  if (*p != '{') return NULL;
  ++p;

  char keyBuf[PT_JSON_MAX_KEY + 1];

  for (;;) {
    p = ptJsonSkipWs(p);
    if (*p == '\0') return NULL;
    if (*p == '}') return NULL;           /* end of object, key absent */
    if (*p == ',') { ++p; continue; }
    if (*p != '"') return NULL;           /* malformed: expected a key */

    p = ptJsonReadString(p, keyBuf, sizeof(keyBuf));
    if (p == NULL) return NULL;

    p = ptJsonSkipWs(p);
    if (*p != ':') return NULL;
    ++p;
    p = ptJsonSkipWs(p);

    if (strcmp(keyBuf, key) == 0) return p;

    p = ptJsonSkipValue(p);
    if (p == NULL) return NULL;
  }
}

inline bool ptJsonHas(const char* json, const char* key) {
  return ptJsonFind(json, key) != NULL;
}

inline bool ptJsonGetStr(const char* json, const char* key,
                         char* out, size_t outCap) {
  const char* v = ptJsonFind(json, key);
  if (v == NULL || *v != '"') return false;
  return ptJsonReadString(v, out, outCap) != NULL;
}

inline bool ptJsonGetLong(const char* json, const char* key, long* out) {
  const char* v = ptJsonFind(json, key);
  if (v == NULL || out == NULL) return false;
  /* Reject the non-numeric literals explicitly. strtol would return 0 for
   * "null" and "false", silently turning a malformed payload into a plausible
   * value, which is far harder to debug than a clean failure. */
  if (!((*v >= '0' && *v <= '9') || *v == '-' || *v == '+')) return false;
  char* end = NULL;
  long parsed = strtol(v, &end, 10);
  if (end == v) return false;
  *out = parsed;
  return true;
}

inline bool ptJsonGetULong(const char* json, const char* key, unsigned long* out) {
  const char* v = ptJsonFind(json, key);
  if (v == NULL || out == NULL) return false;
  if (!(*v >= '0' && *v <= '9')) return false;   /* unsigned: no sign allowed */
  char* end = NULL;
  unsigned long parsed = strtoul(v, &end, 10);
  if (end == v) return false;
  *out = parsed;
  return true;
}

inline bool ptJsonGetInt(const char* json, const char* key, int* out) {
  long v;
  if (!ptJsonGetLong(json, key, &v)) return false;
  *out = (int)v;
  return true;
}

inline bool ptJsonGetBool(const char* json, const char* key, bool* out) {
  const char* v = ptJsonFind(json, key);
  if (v == NULL || out == NULL) return false;
  if (strncmp(v, "true", 4) == 0)  { *out = true;  return true; }
  if (strncmp(v, "false", 5) == 0) { *out = false; return true; }
  return false;
}

#endif /* PT_JSON_H */
