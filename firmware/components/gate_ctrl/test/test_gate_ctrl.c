/*
 * Unity tests for gate_ctrl: the full path from keypress to "open the
 * door", driven through real access_core and ui instances with a
 * deterministic fake hash.
 */
#include <string.h>

#include "gate_ctrl.h"
#include "unity.h"

#define T0 1773360000LL
#define DAY 86400LL
#define MAXF 5
#define LOCK_S 300

static ac_ctx_t ac;
static ui_ctx_t ui;
static gate_ctrl_t g;

/* Injective on 9-character codes, which is all a test needs. */
static void fake_hash(const char *code, uint8_t out[AC_HASH_LEN], void *user)
{
    (void)user;
    memset(out, 0xA5, AC_HASH_LEN);
    memcpy(out, code, strlen(code));
}

static ui_now_t at(uint32_t ms, int64_t epoch)
{
    ui_now_t n = {.ms = ms, .epoch = epoch};
    return n;
}

static const char *GOOD = "123456AB*";

static void gate_fixture(void)
{
    ac_init(&ac, MAXF, LOCK_S);
    ac.clock_trusted = true;
    uint8_t h[AC_HASH_LEN];
    fake_hash(GOOD, h, NULL);
    ac_upsert(&ac, "guest001", h, T0, T0 + DAY);

    ui_init(&ui);
    ui_set_clock_trusted(&ui, true);
    gate_init(&g, &ac, &ui, fake_hash, NULL);
}

/* Types a whole string; returns the output of the last key. */
static gate_out_t type(const char *s, uint32_t *ms, int64_t epoch)
{
    gate_out_t out;
    memset(&out, 0, sizeof(out));
    for (; *s; s++) {
        out = gate_on_key(&g, *s, at(*ms, epoch));
        *ms += 200;
    }
    return out;
}

static bool buf_is_wiped(void)
{
    for (size_t i = 0; i < sizeof(g.buf); i++) {
        if (g.buf[i] != 0) {
            return false;
        }
    }
    return true;
}

TEST_CASE("a correct code opens the lock", "[gate]")
{
    gate_fixture();
    uint32_t ms = 0;
    gate_out_t out = type(GOOD, &ms, T0 + 60);
    TEST_ASSERT_TRUE(out.submitted);
    TEST_ASSERT_EQUAL(AC_GRANTED, out.result);
    TEST_ASSERT_TRUE(out.open_lock);
    TEST_ASSERT_EQUAL_STRING("guest001", out.matched_id);
}

TEST_CASE("nothing opens before the ninth key", "[gate]")
{
    gate_fixture();
    uint32_t ms = 0;
    gate_out_t out = type("123456AB", &ms, T0 + 60);
    TEST_ASSERT_FALSE(out.submitted);
    TEST_ASSERT_FALSE(out.open_lock);
}

TEST_CASE("the typed code is wiped after a grant", "[gate]")
{
    gate_fixture();
    uint32_t ms = 0;
    type(GOOD, &ms, T0 + 60);
    TEST_ASSERT_TRUE(buf_is_wiped());
}

TEST_CASE("the typed code is wiped after a denial", "[gate]")
{
    gate_fixture();
    uint32_t ms = 0;
    type("999999CD#", &ms, T0 + 60);
    TEST_ASSERT_TRUE(buf_is_wiped());
}

TEST_CASE("a wrong code does not open and is persisted", "[gate]")
{
    gate_fixture();
    uint32_t ms = 0;
    gate_out_t out = type("999999CD#", &ms, T0 + 60);
    TEST_ASSERT_EQUAL(AC_DENIED_UNKNOWN, out.result);
    TEST_ASSERT_FALSE(out.open_lock);
    TEST_ASSERT_TRUE(out.persist_attempts);
    TEST_ASSERT_EQUAL_UINT8(1, ac.failed_attempts);
}

TEST_CASE("a malformed code still costs an attempt", "[gate]")
{
    gate_fixture();
    uint32_t ms = 0;
    /* Nine digits: well-formed length, wrong composition. */
    gate_out_t out = type("123456789", &ms, T0 + 60);
    TEST_ASSERT_TRUE(out.submitted);
    TEST_ASSERT_FALSE(out.open_lock);
    TEST_ASSERT_EQUAL_UINT8(1, ac.failed_attempts);
}

TEST_CASE("a genuine code outside its window neither opens nor persists", "[gate]")
{
    gate_fixture();
    uint32_t ms = 0;
    gate_out_t out = type(GOOD, &ms, T0 - 3600); /* an hour early */
    TEST_ASSERT_EQUAL(AC_DENIED_NOT_YET, out.result);
    TEST_ASSERT_FALSE(out.open_lock);
    TEST_ASSERT_FALSE(out.persist_attempts);
}

TEST_CASE("a long press discards the partial entry", "[gate]")
{
    gate_fixture();
    uint32_t ms = 0;
    type("9999", &ms, T0 + 60);
    gate_on_long_press(&g, at(ms, T0 + 60));
    TEST_ASSERT_TRUE(buf_is_wiped());

    /* And the next full code is evaluated clean, not with a prefix. */
    gate_out_t out = type(GOOD, &ms, T0 + 60);
    TEST_ASSERT_EQUAL(AC_GRANTED, out.result);
}

TEST_CASE("an abandoned entry is wiped on timeout", "[gate]")
{
    gate_fixture();
    uint32_t ms = 0;
    type("12345", &ms, T0 + 60);
    TEST_ASSERT_FALSE(buf_is_wiped());
    gate_tick(&g, at(ms + UI_ENTRY_TIMEOUT_MS, T0 + 60));
    TEST_ASSERT_TRUE(buf_is_wiped());
}

TEST_CASE("keys are ignored during a lockout", "[gate]")
{
    gate_fixture();
    uint32_t ms = 0;
    for (int i = 0; i < MAXF; i++) {
        type("999999CD#", &ms, T0 + 60);
    }
    TEST_ASSERT_TRUE(ac_is_locked_out(&ac, T0 + 60));

    /* Even the right code does nothing, and nothing is buffered. */
    gate_out_t out = type(GOOD, &ms, T0 + 61);
    TEST_ASSERT_FALSE(out.submitted);
    TEST_ASSERT_FALSE(out.open_lock);
    TEST_ASSERT_TRUE(buf_is_wiped());
}

TEST_CASE("the lifted lockout is persisted", "[gate]")
{
    gate_fixture();
    uint32_t ms = 0;
    for (int i = 0; i < MAXF; i++) {
        type("999999CD#", &ms, T0 + 60);
    }
    /* If this were not persisted, a reboot would restore a counter at
     * the maximum and a single slip would lock the gate again. */
    gate_out_t out = gate_tick(&g, at(ms, T0 + 60 + LOCK_S + 1));
    TEST_ASSERT_TRUE(out.persist_attempts);
    TEST_ASSERT_EQUAL_UINT8(0, ac.failed_attempts);
}

TEST_CASE("ticks that change nothing ask for no persistence", "[gate]")
{
    gate_fixture();
    for (uint32_t ms = 0; ms < 5000; ms += 20) {
        TEST_ASSERT_FALSE(gate_tick(&g, at(ms, T0 + 60)).persist_attempts);
    }
}

TEST_CASE("without trusted time, keys enter nothing", "[gate]")
{
    gate_fixture();
    ui_set_clock_trusted(&ui, false);
    ac.clock_trusted = false;
    uint32_t ms = 0;
    gate_out_t out = type(GOOD, &ms, T0 + 60);
    TEST_ASSERT_FALSE(out.submitted);
    TEST_ASSERT_TRUE(buf_is_wiped());
}
