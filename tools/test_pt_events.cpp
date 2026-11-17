/*
 * PhishTap Lite — host-side unit test for pt_events.h
 *
 * WHY THE RING DESERVES ITS OWN TESTS
 * The ring's bookkeeping is pure integer arithmetic over sequence numbers, and
 * it is the one place in the firmware where an off-by-one is both invisible and
 * consequential. Get `oldestSeq` wrong by one and a card tap is silently
 * dropped: the console looks healthy, Python never hears about the tap, and the
 * only symptom is a student waving a card at a machine that ignores them. That
 * is not a bug anyone can diagnose from the outside, so it is tested from the
 * inside here instead.
 *
 * pt_events.h has no Arduino dependency for exactly this reason. Timestamps are
 * passed in, so the real production code runs under g++ on a laptop.
 *
 * The --dump mode prints a representative full-ring payload to stdout. The gate
 * pipes that through Python's json module, which is an INDEPENDENT parser: it
 * proves the bytes this firmware emits are valid JSON by a standard that has
 * nothing to do with the code that wrote them.
 *
 * Run via tools/host_gate.sh. Never pushed to the board.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../sketch/phishtap/pt_events.h"

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

/* ── Emptiness and basic push ───────────────────────────────────────────── */

static void test_ring_empty(void) {
  SECTION("ring: a fresh ring is empty");
  PtEventRing r;
  ptRingInit(&r);

  CHECK(ptRingEmpty(&r), "fresh ring reports empty");
  CHECK(ptRingLen(&r) == 0, "fresh ring length is 0");
  CHECK(r.oldestSeq == 1, "fresh oldestSeq is 1");
  CHECK(r.newestSeq == 0, "fresh newestSeq is 0");
  CHECK(r.overflows == 0, "fresh ring has no overflows");

  /* Draining an empty ring must produce a valid empty array, not garbage. */
  char buf[PT_JSON_EVENTS];
  bool missed = true;
  size_t n = ptEventsWriteJson(&r, 0, buf, sizeof(buf), &missed);
  CHECK(n > 0, "empty ring still serialises");
  CHECK_STR_EQ(buf, "{\"events\":[],\"oldest_seq\":0,\"overflow\":false}",
               "empty ring payload");
  CHECK(missed == false, "empty ring reports no missed events");
}

static void test_ring_push(void) {
  SECTION("ring: push assigns monotonic seq starting at 1");
  PtEventRing r;
  ptRingInit(&r);

  uint32_t s1 = ptRingPushHeartbeat(&r, 100);
  CHECK(s1 == 1, "first event gets seq 1, never 0");
  CHECK(ptRingLen(&r) == 1, "length 1 after one push");
  CHECK(!ptRingEmpty(&r), "not empty after one push");
  CHECK(r.oldestSeq == 1, "oldestSeq unchanged by the first push");
  CHECK(r.newestSeq == 1, "newestSeq is 1");

  uint32_t s2 = ptRingPushHeartbeat(&r, 200);
  uint32_t s3 = ptRingPushHeartbeat(&r, 300);
  CHECK(s2 == 2 && s3 == 3, "seq increments by one each push");
  CHECK(ptRingLen(&r) == 3, "length 3 after three pushes");
  CHECK(r.totalPushed == 3, "totalPushed counts every push");
}

/* ── Wraparound, the part most likely to be wrong ───────────────────────── */

static void test_ring_wrap(void) {
  SECTION("ring: filling exactly, then wrapping");
  PtEventRing r;
  ptRingInit(&r);

  for (uint32_t i = 1; i <= PT_EVENT_RING_SIZE; ++i) {
    ptRingPushHeartbeat(&r, i * 100);
  }
  CHECK(ptRingLen(&r) == PT_EVENT_RING_SIZE, "exactly full at 16");
  CHECK(r.oldestSeq == 1, "oldest still 1 when exactly full");
  CHECK(r.newestSeq == PT_EVENT_RING_SIZE, "newest is 16");
  CHECK(r.overflows == 0, "no drops while it still fits");

  /* The 17th push must evict seq 1 and nothing else. */
  uint32_t s = ptRingPushHeartbeat(&r, 1700);
  CHECK(s == 17, "17th event gets seq 17");
  CHECK(ptRingLen(&r) == PT_EVENT_RING_SIZE, "length stays capped at 16");
  CHECK(r.oldestSeq == 2, "oldest advanced to 2, evicting seq 1");
  CHECK(r.newestSeq == 17, "newest is 17");
  CHECK(r.overflows == 1, "exactly one drop recorded");

  /* Keep going and confirm the counters track precisely. */
  for (uint32_t i = 0; i < 9; ++i) ptRingPushHeartbeat(&r, 2000 + i);
  CHECK(r.newestSeq == 26, "newest is 26 after 26 pushes");
  CHECK(r.oldestSeq == 11, "oldest is 11, so 11..26 are retained");
  CHECK(ptRingLen(&r) == PT_EVENT_RING_SIZE, "still exactly 16 retained");
  CHECK(r.overflows == 10, "10 drops for 26 pushes into a 16 slot ring");
  CHECK(r.totalPushed == 26, "totalPushed is 26");

  /* The retained slot must actually hold the event its seq claims. */
  const PtEvent* e = &r.slots[r.oldestSeq % PT_EVENT_RING_SIZE];
  CHECK(e->seq == r.oldestSeq, "oldest slot holds the oldest seq");
  const PtEvent* n = &r.slots[r.newestSeq % PT_EVENT_RING_SIZE];
  CHECK(n->seq == r.newestSeq, "newest slot holds the newest seq");
}

/* ── Draining with a cursor ─────────────────────────────────────────────── */

static void test_ring_since(void) {
  SECTION("ring: the since cursor selects the right window");
  PtEventRing r;
  ptRingInit(&r);
  for (uint32_t i = 1; i <= 5; ++i) ptRingPushHeartbeat(&r, i * 100);

  char buf[PT_JSON_EVENTS];
  bool missed = true;

  /* since=0 means "everything from the beginning". */
  size_t n = ptEventsWriteJson(&r, 0, buf, sizeof(buf), &missed);
  CHECK(n > 0, "since=0 serialises");
  CHECK(missed == false, "since=0 never reports missed events");
  CHECK(strstr(buf, "\"seq\":1") != NULL, "since=0 includes seq 1");
  CHECK(strstr(buf, "\"seq\":5") != NULL, "since=0 includes seq 5");

  /* since=3 means "I have 3, give me 4 and 5". */
  n = ptEventsWriteJson(&r, 3, buf, sizeof(buf), &missed);
  CHECK(n > 0, "since=3 serialises");
  CHECK(strstr(buf, "\"seq\":3") == NULL, "since=3 excludes seq 3 itself");
  CHECK(strstr(buf, "\"seq\":4") != NULL, "since=3 includes seq 4");
  CHECK(strstr(buf, "\"seq\":5") != NULL, "since=3 includes seq 5");
  CHECK(missed == false, "no gap when the cursor is inside the ring");

  /* since=newest means "nothing new". */
  n = ptEventsWriteJson(&r, 5, buf, sizeof(buf), &missed);
  CHECK(n > 0, "since=newest serialises");
  CHECK_STR_EQ(buf, "{\"events\":[],\"oldest_seq\":1,\"overflow\":false}",
               "since=newest yields an empty array");

  /* since beyond newest must not wrap into nonsense or emit old events. */
  n = ptEventsWriteJson(&r, 999, buf, sizeof(buf), &missed);
  CHECK(n > 0, "since beyond newest serialises");
  CHECK(strstr(buf, "\"events\":[]") != NULL, "since beyond newest is empty");
}

static void test_ring_missed(void) {
  SECTION("ring: a stale cursor is reported as missed");
  PtEventRing r;
  ptRingInit(&r);
  /* Push 20 into a 16 slot ring, so seqs 1..4 are gone and 5..20 remain. */
  for (uint32_t i = 1; i <= 20; ++i) ptRingPushHeartbeat(&r, i * 10);
  CHECK(r.oldestSeq == 5, "oldest is 5 after 20 pushes");

  char buf[PT_JSON_EVENTS];
  bool missed = false;

  /* Python last saw seq 2, which the ring no longer holds. */
  size_t n = ptEventsWriteJson(&r, 2, buf, sizeof(buf), &missed);
  CHECK(n > 0, "stale cursor still serialises");
  CHECK(missed == true, "stale cursor sets the overflow flag");
  CHECK(strstr(buf, "\"seq\":3") == NULL, "dropped seq 3 is not invented");
  CHECK(strstr(buf, "\"seq\":5") != NULL, "resync starts at the oldest retained");
  CHECK(strstr(buf, "\"overflow\":true") != NULL, "overflow flag is in the payload");

  /* A cursor at exactly oldest-1 is not a gap: nothing between was dropped. */
  missed = true;
  n = ptEventsWriteJson(&r, 4, buf, sizeof(buf), &missed);
  CHECK(n > 0, "cursor at oldest-1 serialises");
  CHECK(missed == false, "cursor at oldest-1 is not a gap");

  /* A current cursor is not a gap either. */
  missed = true;
  n = ptEventsWriteJson(&r, 20, buf, sizeof(buf), &missed);
  CHECK(missed == false, "current cursor reports no gap");
}

/* ── Acking ─────────────────────────────────────────────────────────────── */

static void test_ring_ack(void) {
  SECTION("ring: ack reclaims slots without corrupting the range");
  PtEventRing r;
  ptRingInit(&r);
  for (uint32_t i = 1; i <= 5; ++i) ptRingPushHeartbeat(&r, i * 100);

  ptRingAck(&r, 3);
  CHECK(r.oldestSeq == 4, "ack(3) moves oldest to 4");
  CHECK(ptRingLen(&r) == 2, "two events remain after acking 3");
  CHECK(r.newestSeq == 5, "ack does not touch newestSeq");

  /* Acking the same or an older seq must not move backwards. */
  ptRingAck(&r, 2);
  CHECK(r.oldestSeq == 4, "ack of an older seq does not rewind oldest");
  ptRingAck(&r, 3);
  CHECK(r.oldestSeq == 4, "re-ack of the same seq is idempotent");

  /* Acking everything empties the ring. */
  ptRingAck(&r, 5);
  CHECK(ptRingEmpty(&r), "acking the newest empties the ring");
  CHECK(ptRingLen(&r) == 0, "length 0 after a full ack");

  /* And the ring must still work afterwards, which is where a naive
   * implementation breaks: the next push has to reuse the sequence correctly
   * rather than restarting at 1. */
  uint32_t s = ptRingPushHeartbeat(&r, 900);
  CHECK(s == 6, "push after a full ack continues the sequence at 6");
  CHECK(ptRingLen(&r) == 1, "length 1 after the post-ack push");
  CHECK(!ptRingEmpty(&r), "not empty after the post-ack push");

  char buf[PT_JSON_EVENTS];
  bool missed = true;
  size_t n = ptEventsWriteJson(&r, 5, buf, sizeof(buf), &missed);
  CHECK(n > 0, "post-ack drain serialises");
  CHECK(strstr(buf, "\"seq\":6") != NULL, "post-ack event is delivered");
  CHECK(missed == false, "post-ack drain reports no gap");

  /* Acking 0 is a no-op, since 0 is the "before everything" cursor. */
  PtEventRing r2;
  ptRingInit(&r2);
  ptRingPushHeartbeat(&r2, 100);
  ptRingAck(&r2, 0);
  CHECK(r2.oldestSeq == 1, "ack(0) is ignored");
  CHECK(ptRingLen(&r2) == 1, "ack(0) does not drop the event");

  /* Acking far beyond newest must clamp, not invent a huge oldestSeq. */
  PtEventRing r3;
  ptRingInit(&r3);
  ptRingPushHeartbeat(&r3, 100);
  ptRingAck(&r3, 100000);
  CHECK(r3.oldestSeq == r3.newestSeq + 1, "ack beyond newest clamps to empty");
  CHECK(ptRingEmpty(&r3), "ack beyond newest leaves the ring empty");
  CHECK(ptRingLen(&r3) == 0, "clamped ring has length 0");
  uint32_t s3 = ptRingPushHeartbeat(&r3, 200);
  CHECK(s3 == 2, "sequence continues correctly after a clamping ack");
}

/* ── Per-type payloads ──────────────────────────────────────────────────── */

static void test_event_payloads(void) {
  SECTION("events: each type carries the right data fields");
  PtEventRing r;
  ptRingInit(&r);
  char buf[PT_JSON_EVENTS];
  bool missed = false;

  ptRingPushCardTap(&r, 1234, "C7DA4104", 4);
  size_t n = ptEventsWriteJson(&r, 0, buf, sizeof(buf), &missed);
  CHECK(n > 0, "card_tap serialises");
  CHECK(strstr(buf, "\"type\":\"card_tap\"") != NULL, "card_tap type name");
  CHECK(strstr(buf, "\"uid_hex\":\"C7DA4104\"") != NULL, "card_tap uid_hex");
  CHECK(strstr(buf, "\"uid_len\":4") != NULL, "card_tap uid_len");
  CHECK(strstr(buf, "\"t_ms\":1234") != NULL, "card_tap t_ms");
  ptRingInit(&r);

  ptRingPushCardRemoved(&r, 50);
  n = ptEventsWriteJson(&r, 0, buf, sizeof(buf), &missed);
  CHECK(n > 0, "card_removed serialises");
  CHECK(strstr(buf, "\"type\":\"card_removed\"") != NULL, "card_removed type name");
  /* data must still be present as an empty object so Python can index it
   * unconditionally instead of special-casing this one type. */
  CHECK(strstr(buf, "\"data\":{}") != NULL, "card_removed has an empty data object");
  ptRingInit(&r);

  ptRingPushRoundTimeout(&r, 60, 7);
  n = ptEventsWriteJson(&r, 0, buf, sizeof(buf), &missed);
  CHECK(strstr(buf, "\"type\":\"round_timeout\"") != NULL, "round_timeout type name");
  CHECK(strstr(buf, "\"round\":7") != NULL, "round_timeout carries the round");
  ptRingInit(&r);

  ptRingPushButton(&r, 70, 1, 70123UL);
  n = ptEventsWriteJson(&r, 0, buf, sizeof(buf), &missed);
  CHECK(strstr(buf, "\"type\":\"button\"") != NULL, "button type name");
  CHECK(strstr(buf, "\"id\":1") != NULL, "button carries its id");
  CHECK(strstr(buf, "\"t_us\":70123") != NULL, "button carries its micros timestamp");
  ptRingInit(&r);

  ptRingPushHwFault(&r, 80, PT_FAULT_RFID_INIT);
  n = ptEventsWriteJson(&r, 0, buf, sizeof(buf), &missed);
  CHECK(strstr(buf, "\"type\":\"hw_fault\"") != NULL, "hw_fault type name");
  CHECK(strstr(buf, "\"code\":\"E_RFID_INIT\"") != NULL, "hw_fault carries its code name");
  ptRingInit(&r);

  ptRingPushHeartbeat(&r, 9000);
  n = ptEventsWriteJson(&r, 0, buf, sizeof(buf), &missed);
  CHECK(strstr(buf, "\"type\":\"heartbeat\"") != NULL, "heartbeat type name");
  CHECK(strstr(buf, "\"uptime_ms\":9000") != NULL, "heartbeat carries uptime");
  (void)n;
}

static void test_uid_truncation(void) {
  SECTION("events: an over-long UID truncates instead of overrunning");
  PtEventRing r;
  ptRingInit(&r);

  /* Longer than PT_UID_HEX_MAX. A card should never report this, but a
   * corrupted read could, and the copy must be bounded regardless. */
  char huge[PT_UID_HEX_MAX * 3];
  memset(huge, 'A', sizeof(huge) - 1);
  huge[sizeof(huge) - 1] = '\0';

  ptRingPushCardTap(&r, 10, huge, 255);
  const PtEvent* e = &r.slots[r.newestSeq % PT_EVENT_RING_SIZE];
  CHECK(strlen(e->uid_hex) == PT_UID_HEX_MAX, "uid_hex truncated to the field size");
  CHECK(e->uid_hex[PT_UID_HEX_MAX] == '\0', "uid_hex stays NUL terminated");

  char buf[PT_JSON_EVENTS];
  bool missed = false;
  CHECK(ptEventsWriteJson(&r, 0, buf, sizeof(buf), &missed) > 0,
        "truncated uid still serialises");
}

static void test_buffer_too_small(void) {
  SECTION("events: a too-small buffer returns 0 rather than truncating");
  PtEventRing r;
  ptRingInit(&r);
  for (uint32_t i = 0; i < PT_EVENT_RING_SIZE; ++i) {
    ptRingPushCardTap(&r, i * 100, "C7DA4104", 4);
  }

  char small[64];
  bool missed = false;
  size_t n = ptEventsWriteJson(&r, 0, small, sizeof(small), &missed);
  CHECK(n == 0, "a full ring does not fit in 64 bytes and says so");
  CHECK(small[0] == '\0', "buffer cleared rather than left half written");
  CHECK(strlen(small) < sizeof(small), "no overrun of the small buffer");

  /* The same ring must fit in the real buffer. */
  char full[PT_JSON_EVENTS];
  n = ptEventsWriteJson(&r, 0, full, sizeof(full), &missed);
  CHECK(n > 0, "a full ring fits in PT_JSON_EVENTS");
  CHECK(n < sizeof(full), "and leaves headroom");
  printf("     (a full 16-event card_tap ring serialises to %u bytes of %u)\n",
         (unsigned)n, (unsigned)sizeof(full));
}

/* ── Independent validation ─────────────────────────────────────────────── */

static void test_envelope_is_readable(void) {
  SECTION("events: the envelope is readable by our own reader");
  PtEventRing r;
  ptRingInit(&r);
  ptRingPushHeartbeat(&r, 1000);
  ptRingPushCardTap(&r, 1100, "C7DA4104", 4);

  char buf[PT_JSON_EVENTS];
  bool missed = true;
  CHECK(ptEventsWriteJson(&r, 0, buf, sizeof(buf), &missed) > 0, "serialises");

  /* The top-level envelope keys must be reachable. The nested event objects are
   * deliberately NOT reachable through ptJsonFind, which is the documented
   * scope rule, so only the envelope is asserted here. */
  unsigned long oldest = 999;
  CHECK(ptJsonGetULong(buf, "oldest_seq", &oldest), "oldest_seq is readable");
  CHECK(oldest == 1, "oldest_seq value");
  bool ov = true;
  CHECK(ptJsonGetBool(buf, "overflow", &ov), "overflow is readable");
  CHECK(ov == false, "overflow value");
  CHECK(ptJsonHas(buf, "events"), "events key present");

  /* Structural sanity: exactly one opening and closing bracket pair for the
   * array, and every event object closed. A cheap check, but it catches the
   * unbalanced-container bug that would otherwise only show up as a Python
   * decode error. */
  int braces = 0, brackets = 0;
  bool inStr = false;
  for (const char* p = buf; *p != '\0'; ++p) {
    if (*p == '\\' && inStr) { ++p; continue; }
    if (*p == '"') { inStr = !inStr; continue; }
    if (inStr) continue;
    if (*p == '{') ++braces;
    if (*p == '}') --braces;
    if (*p == '[') ++brackets;
    if (*p == ']') --brackets;
    CHECK(braces >= 0, "objects never close more than they open");
    CHECK(brackets >= 0, "arrays never close more than they open");
  }
  CHECK(braces == 0, "every object is closed");
  CHECK(brackets == 0, "every array is closed");
}

/* --dump: print a representative payload for validation by Python's json
 * module. This is the strongest check available, because that parser shares no
 * code with the writer and implements the real RFC 8259 grammar. */
static int dump_payload(void) {
  PtEventRing r;
  ptRingInit(&r);

  ptRingPushHeartbeat(&r, 1000);
  ptRingPushCardTap(&r, 1500, "C7DA4104", 4);
  ptRingPushCardRemoved(&r, 2500);
  ptRingPushRoundTimeout(&r, 3000, 4);
  ptRingPushButton(&r, 3500, 1, 3500123UL);
  ptRingPushHwFault(&r, 4000, PT_FAULT_RFID_READ);
  ptRingPushHeartbeat(&r, 5000);
  /* A UID containing characters that must be escaped if they ever appeared. */
  ptRingPushCardTap(&r, 5500, "0A\"F\\01", 4);

  char buf[PT_JSON_EVENTS];
  bool missed = false;
  size_t n = ptEventsWriteJson(&r, 0, buf, sizeof(buf), &missed);
  if (n == 0) {
    fprintf(stderr, "dump: payload did not fit\n");
    return 1;
  }
  fwrite(buf, 1, n, stdout);
  fputc('\n', stdout);
  return 0;
}

int main(int argc, char** argv) {
  if (argc > 1 && strcmp(argv[1], "--dump") == 0) return dump_payload();

  printf("pt_events.h host unit test\n");
  printf("==========================\n");

  test_ring_empty();
  test_ring_push();
  test_ring_wrap();
  test_ring_since();
  test_ring_missed();
  test_ring_ack();
  test_event_payloads();
  test_uid_truncation();
  test_buffer_too_small();
  test_envelope_is_readable();

  printf("\n==========================\n");
  printf("passed: %d   failed: %d\n", g_pass, g_fail);
  if (g_fail == 0) {
    printf("RESULT: PASS\n");
    return 0;
  }
  printf("RESULT: FAIL\n");
  return 1;
}
