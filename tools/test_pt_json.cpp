/*
 * PhishTap Lite — host-side unit test for pt_json.h
 *
 * WHY THIS EXISTS
 * pt_json.h is deliberately free of any Arduino dependency so that it can be
 * compiled and executed by plain g++ on a laptop. That turns the JSON codec from
 * "code we hope is right" into "code with passing tests", which matters because
 * a malformed bridge payload fails silently and confusingly: Python raises a
 * decode error, or worse, reads a plausible wrong value, and the only visible
 * symptom is the game misbehaving.
 *
 * Run it with tools/json_gate.sh, which also compiles it under -Wall -Wextra
 * -Werror so a warning cannot slip through.
 *
 * This file is NOT part of the sketch and is never pushed to the board.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../sketch/phishtap/pt_json.h"

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, msg)                                                    \
  do {                                                                      \
    if (cond) { ++g_pass; }                                                 \
    else {                                                                  \
      ++g_fail;                                                             \
      printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, (msg));              \
    }                                                                       \
  } while (0)

#define CHECK_STR_EQ(got, want, msg)                                        \
  do {                                                                      \
    if (strcmp((got), (want)) == 0) { ++g_pass; }                           \
    else {                                                                  \
      ++g_fail;                                                             \
      printf("  FAIL %s:%d  %s\n       got:  [%s]\n       want: [%s]\n",    \
             __FILE__, __LINE__, (msg), (got), (want));                     \
    }                                                                       \
  } while (0)

#define SECTION(name) printf("\n-- %s\n", name)

/* ── Writer ─────────────────────────────────────────────────────────────── */

static void test_writer_basics(void) {
  SECTION("writer: basics");
  char buf[256];
  PtJsonWriter w;

  ptJsonInit(&w, buf, sizeof(buf));
  ptJsonObjBegin(&w);
  ptJsonObjEnd(&w);
  CHECK(ptJsonFinish(&w) == 2, "empty object is {}");
  CHECK_STR_EQ(buf, "{}", "empty object text");

  ptJsonInit(&w, buf, sizeof(buf));
  ptJsonObjBegin(&w);
  ptJsonKVBool(&w, "ok", true);
  ptJsonObjEnd(&w);
  CHECK(ptJsonFinish(&w) > 0, "single bool fits");
  CHECK_STR_EQ(buf, "{\"ok\":true}", "single bool text");

  ptJsonInit(&w, buf, sizeof(buf));
  ptJsonObjBegin(&w);
  ptJsonKVInt(&w, "a", 1);
  ptJsonKVInt(&w, "b", -2);
  ptJsonKVUint(&w, "c", 4294967295UL);
  ptJsonKVStr(&w, "d", "x");
  ptJsonKVBool(&w, "e", false);
  ptJsonObjEnd(&w);
  CHECK(ptJsonFinish(&w) > 0, "multi-field object fits");
  CHECK_STR_EQ(buf, "{\"a\":1,\"b\":-2,\"c\":4294967295,\"d\":\"x\",\"e\":false}",
               "commas separate every field exactly once");
}

static void test_writer_escaping(void) {
  SECTION("writer: string escaping");
  char buf[256];
  PtJsonWriter w;

  ptJsonInit(&w, buf, sizeof(buf));
  ptJsonObjBegin(&w);
  ptJsonKVStr(&w, "s", "a\"b\\c");
  ptJsonObjEnd(&w);
  CHECK(ptJsonFinish(&w) > 0, "escaped string fits");
  CHECK_STR_EQ(buf, "{\"s\":\"a\\\"b\\\\c\"}", "quote and backslash escaped");

  /* A raw control byte is invalid inside a JSON string, so it must become
   * \u00XX. If this ever regresses, Python's json.loads will reject the whole
   * payload. */
  ptJsonInit(&w, buf, sizeof(buf));
  ptJsonObjBegin(&w);
  ptJsonKVStr(&w, "s", "a\x01""b");
  ptJsonObjEnd(&w);
  CHECK(ptJsonFinish(&w) > 0, "control char fits");
  CHECK_STR_EQ(buf, "{\"s\":\"a\\u0001b\"}", "control char escaped as \\u0001");

  ptJsonInit(&w, buf, sizeof(buf));
  ptJsonObjBegin(&w);
  ptJsonKVStr(&w, "s", "line\nbreak\ttab");
  ptJsonObjEnd(&w);
  CHECK_STR_EQ(buf, "{\"s\":\"line\\nbreak\\ttab\"}", "newline and tab escaped");

  /* NULL must not crash; it is written as an empty string. */
  ptJsonInit(&w, buf, sizeof(buf));
  ptJsonObjBegin(&w);
  ptJsonKVStr(&w, "s", NULL);
  ptJsonObjEnd(&w);
  CHECK(ptJsonFinish(&w) > 0, "NULL string does not crash");
  CHECK_STR_EQ(buf, "{\"s\":\"\"}", "NULL string becomes empty");
}

static void test_writer_nesting(void) {
  SECTION("writer: nesting and arrays");
  char buf[256];
  PtJsonWriter w;

  ptJsonInit(&w, buf, sizeof(buf));
  ptJsonObjBegin(&w);
  ptJsonKVInt(&w, "n", 7);
  ptJsonKey(&w, "inner");
  ptJsonObjBegin(&w);
  ptJsonKVStr(&w, "k", "v");
  ptJsonObjEnd(&w);
  ptJsonKVBool(&w, "after", true);
  ptJsonObjEnd(&w);
  CHECK(ptJsonFinish(&w) > 0, "nested object fits");
  CHECK_STR_EQ(buf, "{\"n\":7,\"inner\":{\"k\":\"v\"},\"after\":true}",
               "nested object commas are correct");

  /* The exact shape pt.get_events returns. */
  ptJsonInit(&w, buf, sizeof(buf));
  ptJsonObjBegin(&w);
  ptJsonKey(&w, "events");
  ptJsonArrBegin(&w);
  for (int i = 0; i < 2; ++i) {
    ptJsonObjBegin(&w);
    ptJsonKVUint(&w, "seq", (unsigned long)(i + 1));
    ptJsonKVStr(&w, "type", "heartbeat");
    ptJsonObjEnd(&w);
  }
  ptJsonArrEnd(&w);
  ptJsonKVBool(&w, "overflow", false);
  ptJsonObjEnd(&w);
  CHECK(ptJsonFinish(&w) > 0, "event array fits");
  CHECK_STR_EQ(buf,
    "{\"events\":[{\"seq\":1,\"type\":\"heartbeat\"},"
    "{\"seq\":2,\"type\":\"heartbeat\"}],\"overflow\":false}",
    "array of objects serialises correctly");

  /* An empty array must not gain a stray comma. */
  ptJsonInit(&w, buf, sizeof(buf));
  ptJsonObjBegin(&w);
  ptJsonKey(&w, "events");
  ptJsonArrBegin(&w);
  ptJsonArrEnd(&w);
  ptJsonKVInt(&w, "x", 1);
  ptJsonObjEnd(&w);
  CHECK_STR_EQ(buf, "{\"events\":[],\"x\":1}", "empty array has no comma");
}

static void test_writer_overflow(void) {
  SECTION("writer: overflow is detected, never overruns");
  char buf[16];
  PtJsonWriter w;

  ptJsonInit(&w, buf, sizeof(buf));
  ptJsonObjBegin(&w);
  ptJsonKVStr(&w, "a_very_long_key_name", "and a very long value too");
  ptJsonObjEnd(&w);
  CHECK(ptJsonFinish(&w) == 0, "overflow reports 0 length");
  CHECK(w.overflow, "overflow flag latched");
  CHECK(buf[0] == '\0', "buffer cleared on overflow");
  CHECK(strlen(buf) < sizeof(buf), "no overrun past the buffer");

  /* Exactly-fitting content must succeed, proving the reserve is not off by one
   * in the pessimistic direction. */
  char tight[8];
  ptJsonInit(&w, tight, sizeof(tight));
  ptJsonObjBegin(&w);
  ptJsonKVInt(&w, "a", 1);
  ptJsonObjEnd(&w);
  CHECK(ptJsonFinish(&w) == 7, "{\"a\":1} is 7 chars and fits in 8");
  CHECK_STR_EQ(tight, "{\"a\":1}", "tight buffer content");
}

/* ── Reader ─────────────────────────────────────────────────────────────── */

static void test_reader_scalars(void) {
  SECTION("reader: scalars");
  const char* j = "{\"state\":\"playing\",\"round\":4,\"limit_ms\":12500,"
                  "\"ok\":true,\"since\":0,\"neg\":-17}";
  char s[32];
  long l = 0;
  unsigned long u = 0;
  int i = 0;
  bool b = false;
  memset(s, 0, sizeof(s));

  CHECK(ptJsonGetStr(j, "state", s, sizeof(s)), "state found");
  CHECK_STR_EQ(s, "playing", "state value");

  CHECK(ptJsonGetInt(j, "round", &i), "round found");
  CHECK(i == 4, "round value");

  CHECK(ptJsonGetLong(j, "limit_ms", &l), "limit_ms found");
  CHECK(l == 12500, "limit_ms value");

  CHECK(ptJsonGetULong(j, "since", &u), "since found");
  CHECK(u == 0, "since value is 0");

  CHECK(ptJsonGetLong(j, "neg", &l), "negative found");
  CHECK(l == -17, "negative value");

  CHECK(ptJsonGetBool(j, "ok", &b), "bool found");
  CHECK(b == true, "bool value");

  CHECK(!ptJsonHas(j, "missing"), "absent key reports false");
  CHECK(!ptJsonGetStr(j, "missing", s, sizeof(s)), "absent string fails");
  CHECK(!ptJsonGetInt(j, "missing", &i), "absent int fails");
}

static void test_reader_whitespace(void) {
  SECTION("reader: whitespace tolerance");
  const char* j = "  {  \"a\" : 1 ,  \"b\" : \"two\"  , \"c\":true }  ";
  int i = 0;
  bool b = false;
  char s[16];
  memset(s, 0, sizeof(s));
  CHECK(ptJsonGetInt(j, "a", &i) && i == 1, "spaced int");
  CHECK(ptJsonGetStr(j, "b", s, sizeof(s)) && strcmp(s, "two") == 0, "spaced string");
  CHECK(ptJsonGetBool(j, "c", &b) && b, "spaced bool");

  const char* nl = "{\n  \"x\": 5,\n  \"y\": 6\n}";
  i = 0;
  CHECK(ptJsonGetInt(nl, "y", &i) && i == 6, "newlines and indentation");
}

static void test_reader_key_boundaries(void) {
  SECTION("reader: key matching is exact, not prefix");
  /* "seq" must not match "sequence", and vice versa. A prefix match here would
   * silently return the wrong field's value. */
  const char* j = "{\"sequence\":99,\"seq\":7}";
  long l = 0;
  CHECK(ptJsonGetLong(j, "seq", &l) && l == 7, "exact key wins over prefix");
  CHECK(ptJsonGetLong(j, "sequence", &l) && l == 99, "longer key also found");
  CHECK(!ptJsonHas(j, "sequ"), "partial key not found");
  CHECK(!ptJsonHas(j, "seqq"), "near-miss key not found");

  /* Same key name appearing twice: the FIRST occurrence wins, which is what
   * Python's json module also does for duplicate keys. */
  const char* dup = "{\"a\":1,\"a\":2}";
  CHECK(ptJsonGetLong(dup, "a", &l) && l == 1, "first duplicate wins");
}

static void test_reader_nesting(void) {
  SECTION("reader: nested keys are NOT matched");
  /* This is the core scope guarantee. "inner" contains a key called "state";
   * a top-level lookup for "state" must fail rather than reach inside. */
  const char* j = "{\"outer\":1,\"inner\":{\"state\":\"nested\",\"n\":2},"
                  "\"arr\":[{\"state\":\"inarray\"},3],\"tail\":9}";
  char s[32];
  long l = 0;
  memset(s, 0, sizeof(s));

  CHECK(!ptJsonGetStr(j, "state", s, sizeof(s)),
        "key inside a nested object is not returned");
  CHECK(ptJsonGetLong(j, "outer", &l) && l == 1, "key before nesting found");
  CHECK(ptJsonGetLong(j, "tail", &l) && l == 9,
        "key AFTER a nested object and array is still found");

  /* The nested object itself is present as a key. */
  CHECK(ptJsonHas(j, "inner"), "nested object key is visible");
  CHECK(ptJsonHas(j, "arr"), "array key is visible");
}

static void test_reader_malformed(void) {
  SECTION("reader: malformed input fails cleanly, never crashes");
  char s[32];
  long l = 0;
  memset(s, 0, sizeof(s));

  CHECK(!ptJsonGetLong(NULL, "a", &l), "NULL json");
  CHECK(!ptJsonGetLong("{}", NULL, &l), "NULL key");
  CHECK(!ptJsonGetLong("", "a", &l), "empty string");
  CHECK(!ptJsonGetLong("not json", "a", &l), "not an object");
  CHECK(!ptJsonGetLong("[1,2,3]", "a", &l), "array at top level");
  CHECK(!ptJsonGetLong("{\"a\":", "a", &l), "truncated value");
  CHECK(!ptJsonGetLong("{\"a\"}", "a", &l), "missing colon");
  CHECK(!ptJsonGetLong("{a:1}", "a", &l), "unquoted key");
  CHECK(!ptJsonGetLong("{\"a\":\"unterminated}", "a", &l), "unterminated string");

  /* A number field must reject the literals that strtol would read as 0.
   * Without this, {"round":null} would silently become round 0. */
  CHECK(!ptJsonGetLong("{\"round\":null}", "round", &l), "null rejected as number");
  CHECK(!ptJsonGetLong("{\"round\":false}", "round", &l), "false rejected as number");
  CHECK(!ptJsonGetLong("{\"round\":\"7\"}", "round", &l), "quoted number rejected");

  /* The unsigned getter must reject a sign rather than wrap it, and must leave
   * the caller's variable untouched so a failed read cannot inject a value. */
  unsigned long u = 12345;
  CHECK(!ptJsonGetULong("{\"n\":-5}", "n", &u), "unsigned getter rejects sign");
  CHECK(u == 12345, "unsigned out untouched on failure");
  CHECK(ptJsonGetULong("{\"n\":5}", "n", &u) && u == 5, "unsigned getter accepts digits");

  /* Invalid escape must not desync the scanner. */
  CHECK(!ptJsonGetStr("{\"a\":\"bad\\qescape\"}", "a", s, sizeof(s)),
        "invalid escape rejected");

  /* Deeply nested braces must be skipped without losing the outer scan. */
  const char* deep = "{\"x\":{\"y\":{\"z\":[1,2,{\"w\":\"v\"}]}},\"found\":42}";
  CHECK(ptJsonGetLong(deep, "found", &l) && l == 42, "deep nesting skipped");

  /* A brace inside a string must not be counted as structure. */
  const char* tricky = "{\"s\":\"a } b { c\",\"after\":1}";
  CHECK(ptJsonGetStr(tricky, "s", s, sizeof(s)) && strcmp(s, "a } b { c") == 0,
        "braces inside strings are literal");
  CHECK(ptJsonGetLong(tricky, "after", &l) && l == 1,
        "scan resumes correctly after a string containing braces");

  /* Long keys must be bounded, not overflow keyBuf. */
  char longJson[512];
  char longKey[300];
  memset(longKey, 'k', sizeof(longKey) - 1);
  longKey[sizeof(longKey) - 1] = '\0';
  snprintf(longJson, sizeof(longJson), "{\"%s\":1,\"tail\":2}", longKey);
  CHECK(ptJsonGetLong(longJson, "tail", &l) && l == 2,
        "over-long key does not break the scan or overflow");
}

static void test_reader_escapes(void) {
  SECTION("reader: escape decoding");
  char s[64];
  memset(s, 0, sizeof(s));
  CHECK(ptJsonGetStr("{\"s\":\"a\\\"b\"}", "s", s, sizeof(s)), "escaped quote read");
  CHECK_STR_EQ(s, "a\"b", "escaped quote value");

  CHECK(ptJsonGetStr("{\"s\":\"a\\\\b\"}", "s", s, sizeof(s)), "escaped backslash read");
  CHECK_STR_EQ(s, "a\\b", "escaped backslash value");

  CHECK(ptJsonGetStr("{\"s\":\"a\\nb\"}", "s", s, sizeof(s)), "escaped newline read");
  CHECK_STR_EQ(s, "a\nb", "escaped newline value");

  CHECK(ptJsonGetStr("{\"s\":\"\\u0041\\u0042\"}", "s", s, sizeof(s)), "unicode escape read");
  CHECK_STR_EQ(s, "AB", "unicode escape decoded to ASCII");

  /* Output buffer smaller than the value: must truncate, not overflow. */
  char tiny[4];
  CHECK(ptJsonGetStr("{\"s\":\"abcdefgh\"}", "s", tiny, sizeof(tiny)),
        "small out buffer succeeds");
  CHECK_STR_EQ(tiny, "abc", "small out buffer truncates safely");
  CHECK(strlen(tiny) < sizeof(tiny), "small out buffer stays NUL-terminated");
}

/* ── Round trip ─────────────────────────────────────────────────────────── */

static void test_round_trip(void) {
  SECTION("round trip: what the writer emits, the reader parses");
  char buf[512];
  PtJsonWriter w;

  ptJsonInit(&w, buf, sizeof(buf));
  ptJsonObjBegin(&w);
  ptJsonKVUint(&w, "proto", 1);
  ptJsonKVStr(&w, "fw", "1.0.0");
  ptJsonKVStr(&w, "state", "playing");
  ptJsonKVUint(&w, "uptime_ms", 123456UL);
  ptJsonKVBool(&w, "rfid_ok", true);
  ptJsonKVBool(&w, "matrix_ok", false);
  ptJsonKVInt(&w, "lives", 2);
  ptJsonKVInt(&w, "round", 7);
  ptJsonKVInt(&w, "queue_len", 0);
  ptJsonKVInt(&w, "overflows", 0);
  ptJsonObjEnd(&w);
  CHECK(ptJsonFinish(&w) > 0, "status payload fits");

  /* Parse it back exactly as a bridge handler's counterpart would. */
  char s[32];
  long l = 0;
  bool b = false;
  unsigned long u = 0;
  memset(s, 0, sizeof(s));
  CHECK(ptJsonGetStr(buf, "state", s, sizeof(s)) && strcmp(s, "playing") == 0,
        "round trip state");
  CHECK(ptJsonGetStr(buf, "fw", s, sizeof(s)) && strcmp(s, "1.0.0") == 0,
        "round trip fw");
  CHECK(ptJsonGetULong(buf, "uptime_ms", &u) && u == 123456UL, "round trip uptime");
  CHECK(ptJsonGetLong(buf, "lives", &l) && l == 2, "round trip lives");
  CHECK(ptJsonGetBool(buf, "rfid_ok", &b) && b == true, "round trip rfid_ok");
  CHECK(ptJsonGetBool(buf, "matrix_ok", &b) && b == false, "round trip matrix_ok");
  CHECK(ptJsonGetLong(buf, "proto", &l) && l == 1, "round trip proto");

  /* A uid_hex value with the characters that matter. */
  ptJsonInit(&w, buf, sizeof(buf));
  ptJsonObjBegin(&w);
  ptJsonKey(&w, "data");
  ptJsonObjBegin(&w);
  ptJsonKVStr(&w, "uid_hex", "C7DA4104");
  ptJsonKVInt(&w, "uid_len", 4);
  ptJsonKVUint(&w, "t_ms", 98765UL);
  ptJsonObjEnd(&w);
  ptJsonObjEnd(&w);
  CHECK(ptJsonFinish(&w) > 0, "card_tap payload fits");
  /* Top-level lookup must NOT reach into data{}, per the scope rule. */
  CHECK(!ptJsonGetStr(buf, "uid_hex", s, sizeof(s)),
        "uid_hex is nested, so top-level lookup correctly fails");
  CHECK_STR_EQ(buf,
    "{\"data\":{\"uid_hex\":\"C7DA4104\",\"uid_len\":4,\"t_ms\":98765}}",
    "card_tap payload text");
}

int main(void) {
  printf("pt_json.h host unit test\n");
  printf("========================\n");

  test_writer_basics();
  test_writer_escaping();
  test_writer_nesting();
  test_writer_overflow();
  test_reader_scalars();
  test_reader_whitespace();
  test_reader_key_boundaries();
  test_reader_nesting();
  test_reader_malformed();
  test_reader_escapes();
  test_round_trip();

  printf("\n========================\n");
  printf("passed: %d   failed: %d\n", g_pass, g_fail);
  if (g_fail == 0) {
    printf("RESULT: PASS\n");
    return 0;
  }
  printf("RESULT: FAIL\n");
  return 1;
}
