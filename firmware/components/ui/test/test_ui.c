/*
 * Unity tests for the guest-facing state machine.
 *
 * Since ADR 0004 the ui owns no attempt counter, so every case drives a
 * real ac_ctx_t. That is deliberate: these tests now exercise the same
 * state the gate actually enforces, rather than a display-side copy
 * that could agree with the tests and disagree with the lock.
 */
#include "ui.h"
#include "unity.h"

static ui_ctx_t ctx;
static ac_ctx_t ac;
static uint8_t good_hash[AC_HASH_LEN];
static uint8_t bad_hash[AC_HASH_LEN];

#define T0 1773360000LL
#define DAY 86400LL
#define MAXF 5
#define LOCK_S 300

static ui_now_t at(uint32_t ms, int64_t epoch)
{
    ui_now_t n = {.ms = ms, .epoch = epoch};
    return n;
}

/* No global setUp(): three suites link into one host binary. */
static void ui_fixture(void)
{
    for (int i = 0; i < AC_HASH_LEN; i++) {
        good_hash[i] = (uint8_t)(1 + i);
        bad_hash[i] = (uint8_t)(200 + i);
    }
    ac_init(&ac, MAXF, LOCK_S);
    ac.clock_trusted = true;
    ac_upsert(&ac, "guest001", good_hash, T0, T0 + DAY);

    ui_init(&ctx);
    ui_set_clock_trusted(&ctx, true);
}

/* Drives real denials through access_core so the counter is genuine. */
static void fail_n(int n, int64_t epoch)
{
    for (int i = 0; i < n; i++) {
        ac_result_t r = ac_evaluate(&ac, bad_hash, epoch, NULL);
        ui_on_result(&ctx, &ac, r, at(0, epoch));
    }
}

static void press_n(uint32_t *ms, int n)
{
    for (int i = 0; i < n; i++) {
        ui_on_key(&ctx, &ac, at(*ms, T0));
        *ms += 400;
    }
}

TEST_CASE("boot without trusted time refuses everything", "[ui]")
{
    ui_fixture();
    ui_init(&ctx); /* clock_trusted back to false */

    /* Dark until someone presses a key. */
    ui_render_t r = ui_render(&ctx, &ac, at(0, T0));
    TEST_ASSERT_EQUAL(UI_SCREEN_IDLE, r.screen);
    TEST_ASSERT_FALSE(r.panel_on);

    /* A press explains itself, briefly, and enters nothing. */
    TEST_ASSERT_EQUAL(UI_ACTION_NONE, ui_on_key(&ctx, &ac, at(10, T0)));
    r = ui_render(&ctx, &ac, at(20, T0));
    TEST_ASSERT_EQUAL(UI_SCREEN_NO_CLOCK, r.screen);
    TEST_ASSERT_TRUE(r.panel_on);

    ui_tick(&ctx, &ac, at(10 + UI_INFO_MS, T0));
    TEST_ASSERT_FALSE(ui_render(&ctx, &ac, at(10 + UI_INFO_MS, T0)).panel_on);
}

TEST_CASE("the render payload never carries the entry length", "[ui]")
{
    ui_fixture();
    uint32_t ms = 0;
    press_n(&ms, 4);
    ui_render_t r = ui_render(&ctx, &ac, at(ms, T0));
    TEST_ASSERT_EQUAL(UI_SCREEN_ENTRY, r.screen);
    /* Only a countdown and an attempt count are exposed. If a future
     * change adds a length field here, this test should stop it. */
    TEST_ASSERT_EQUAL_UINT8(0, r.attempts_used);
    TEST_ASSERT_EQUAL_UINT32(0, r.seconds_remaining);
}

TEST_CASE("each keypress refills the countdown bar", "[ui]")
{
    ui_fixture();
    ui_on_key(&ctx, &ac, at(0, T0));
    TEST_ASSERT_EQUAL_UINT16(1000, ui_render(&ctx, &ac, at(0, T0)).progress_permille);
    TEST_ASSERT_EQUAL_UINT16(500, ui_render(&ctx, &ac, at(5000, T0)).progress_permille);

    ui_on_key(&ctx, &ac, at(5000, T0)); /* the visible reset is the feedback */
    TEST_ASSERT_EQUAL_UINT16(1000, ui_render(&ctx, &ac, at(5000, T0)).progress_permille);
}

TEST_CASE("the ninth key submits", "[ui]")
{
    ui_fixture();
    uint32_t ms = 0;
    for (int i = 0; i < UI_CODE_LEN - 1; i++) {
        TEST_ASSERT_EQUAL(UI_ACTION_NONE, ui_on_key(&ctx, &ac, at(ms, T0)));
        ms += 300;
    }
    TEST_ASSERT_EQUAL(UI_ACTION_SUBMIT, ui_on_key(&ctx, &ac, at(ms, T0)));
}

TEST_CASE("ten seconds of silence abandons the entry", "[ui]")
{
    ui_fixture();
    uint32_t ms = 0;
    press_n(&ms, 3);
    TEST_ASSERT_EQUAL(UI_SCREEN_ENTRY, ui_render(&ctx, &ac, at(ms, T0)).screen);

    ms += UI_ENTRY_TIMEOUT_MS;
    TEST_ASSERT_EQUAL(UI_ACTION_CLEAR_BUFFER, ui_tick(&ctx, &ac, at(ms, T0)));
    TEST_ASSERT_EQUAL(UI_SCREEN_IDLE, ui_render(&ctx, &ac, at(ms, T0)).screen);
    /* An abandoned entry is not a wrong guess. */
    TEST_ASSERT_EQUAL_UINT8(0, ui_render(&ctx, &ac, at(ms, T0)).attempts_used);
}

TEST_CASE("long press clears immediately", "[ui]")
{
    ui_fixture();
    uint32_t ms = 0;
    press_n(&ms, 5);
    TEST_ASSERT_EQUAL(UI_ACTION_CLEAR_BUFFER, ui_on_long_press(&ctx, &ac, at(ms, T0)));
    TEST_ASSERT_EQUAL(UI_SCREEN_IDLE, ui_render(&ctx, &ac, at(ms, T0)).screen);

    /* The next nine presses submit, proving the counter really reset. */
    for (int i = 0; i < UI_CODE_LEN - 1; i++) {
        TEST_ASSERT_EQUAL(UI_ACTION_NONE, ui_on_key(&ctx, &ac, at(ms, T0)));
        ms += 100;
    }
    TEST_ASSERT_EQUAL(UI_ACTION_SUBMIT, ui_on_key(&ctx, &ac, at(ms, T0)));
}

TEST_CASE("granted shows OK for three seconds then returns to idle", "[ui]")
{
    ui_fixture();
    ac_result_t r = ac_evaluate(&ac, good_hash, T0, NULL);
    TEST_ASSERT_EQUAL(AC_GRANTED, r);
    ui_on_result(&ctx, &ac, r, at(1000, T0));
    TEST_ASSERT_EQUAL(UI_SCREEN_GRANTED, ui_render(&ctx, &ac, at(1000, T0)).screen);

    ui_tick(&ctx, &ac, at(1000 + UI_GRANTED_MS - 1, T0));
    TEST_ASSERT_EQUAL(UI_SCREEN_GRANTED,
                      ui_render(&ctx, &ac, at(1000 + UI_GRANTED_MS - 1, T0)).screen);

    ui_tick(&ctx, &ac, at(1000 + UI_GRANTED_MS, T0));
    TEST_ASSERT_EQUAL(UI_SCREEN_IDLE,
                      ui_render(&ctx, &ac, at(1000 + UI_GRANTED_MS, T0)).screen);
}

TEST_CASE("wrong codes count up to the maximum then lock out", "[ui]")
{
    ui_fixture();
    for (int i = 1; i < MAXF; i++) {
        fail_n(1, T0);
        ui_render_t r = ui_render(&ctx, &ac, at(0, T0));
        TEST_ASSERT_EQUAL(UI_SCREEN_DENIED, r.screen);
        TEST_ASSERT_EQUAL_UINT8(i, r.attempts_used);
        TEST_ASSERT_EQUAL_UINT8(MAXF, r.attempts_max);
        ui_tick(&ctx, &ac, at(UI_DENIED_MS, T0));
    }

    fail_n(1, T0);
    ui_render_t r = ui_render(&ctx, &ac, at(0, T0));
    TEST_ASSERT_EQUAL(UI_SCREEN_LOCKOUT, r.screen);
    /* The counter is no longer zeroed on arming, so the display can
     * honestly show "5 of 5" while the lockout runs. */
    TEST_ASSERT_EQUAL_UINT8(MAXF, r.attempts_used);
    TEST_ASSERT_EQUAL_UINT32(LOCK_S, r.seconds_remaining);
}

TEST_CASE("the keypad is inert during lockout", "[ui]")
{
    ui_fixture();
    fail_n(MAXF, T0);
    TEST_ASSERT_TRUE(ac_is_locked_out(&ac, T0));

    TEST_ASSERT_EQUAL(UI_ACTION_NONE, ui_on_key(&ctx, &ac, at(1000, T0)));
    TEST_ASSERT_EQUAL(UI_SCREEN_LOCKOUT, ui_render(&ctx, &ac, at(1000, T0)).screen);

    /* Halfway through, the bar is half full. */
    TEST_ASSERT_EQUAL_UINT16(500,
                             ui_render(&ctx, &ac, at(0, T0 + LOCK_S / 2)).progress_permille);
    TEST_ASSERT_EQUAL_UINT16(0, ui_render(&ctx, &ac, at(0, T0 + LOCK_S)).progress_permille);
}

TEST_CASE("lockout releases and clears the counter", "[ui]")
{
    ui_fixture();
    fail_n(MAXF, T0);
    int64_t after = T0 + LOCK_S + 1;

    ac_tick(&ac, after); /* main loop lifts it without needing a keypress */
    ui_tick(&ctx, &ac, at(0, after));

    TEST_ASSERT_FALSE(ac_is_locked_out(&ac, after));
    ui_render_t r = ui_render(&ctx, &ac, at(0, after));
    TEST_ASSERT_EQUAL(UI_SCREEN_IDLE, r.screen);
    TEST_ASSERT_EQUAL_UINT8(0, r.attempts_used);
}

TEST_CASE("a lockout restored from RTC SRAM still holds", "[ui]")
{
    ui_fixture();
    /* Power was cut 100 s into a 300 s lockout. The DS3232 kept the
     * deadline as an absolute epoch, so the reboot must not hand the
     * attacker a clean slate. */
    ac_init(&ac, MAXF, LOCK_S);
    ac.clock_trusted = true;
    ac_restore_attempts(&ac, MAXF, T0 + 200);
    ui_init(&ctx);
    ui_set_clock_trusted(&ctx, true);

    TEST_ASSERT_TRUE(ac_is_locked_out(&ac, T0));
    TEST_ASSERT_EQUAL(UI_ACTION_NONE, ui_on_key(&ctx, &ac, at(100, T0)));
    ui_render_t r = ui_render(&ctx, &ac, at(0, T0));
    TEST_ASSERT_EQUAL(UI_SCREEN_LOCKOUT, r.screen);
    TEST_ASSERT_EQUAL_UINT32(200, r.seconds_remaining);
}

TEST_CASE("a genuine code outside its window costs no attempt", "[ui]")
{
    ui_fixture();
    ac_revoke(&ac, "guest001");
    ac_upsert(&ac, "early001", good_hash, T0 + DAY, T0 + 2 * DAY);

    /* Ten early arrivals must not lock the real guest out. */
    for (int i = 0; i < 10; i++) {
        ac_result_t r = ac_evaluate(&ac, good_hash, T0, NULL);
        TEST_ASSERT_EQUAL(AC_DENIED_NOT_YET, r);
        ui_on_result(&ctx, &ac, r, at(0, T0));
        TEST_ASSERT_EQUAL(UI_SCREEN_NOT_YET, ui_render(&ctx, &ac, at(0, T0)).screen);
        TEST_ASSERT_EQUAL_UINT8(0, ui_render(&ctx, &ac, at(0, T0)).attempts_used);
        ui_tick(&ctx, &ac, at(UI_INFO_MS, T0));
    }
    TEST_ASSERT_FALSE(ac_is_locked_out(&ac, T0));
}

TEST_CASE("a success wipes the accumulated attempts", "[ui]")
{
    ui_fixture();
    fail_n(2, T0);
    TEST_ASSERT_EQUAL_UINT8(2, ui_render(&ctx, &ac, at(0, T0)).attempts_used);

    ac_result_t r = ac_evaluate(&ac, good_hash, T0, NULL);
    TEST_ASSERT_EQUAL(AC_GRANTED, r);
    ui_on_result(&ctx, &ac, r, at(0, T0));
    TEST_ASSERT_EQUAL_UINT8(0, ui_render(&ctx, &ac, at(0, T0)).attempts_used);
}

TEST_CASE("losing trusted time mid-entry fails closed", "[ui]")
{
    ui_fixture();
    uint32_t ms = 0;
    press_n(&ms, 6);
    ui_set_clock_trusted(&ctx, false);

    /* The entry is abandoned and the panel goes dark. */
    TEST_ASSERT_FALSE(ui_render(&ctx, &ac, at(ms, T0)).panel_on);

    /* The next press says why, and still enters nothing. */
    TEST_ASSERT_EQUAL(UI_ACTION_NONE, ui_on_key(&ctx, &ac, at(ms + 100, T0)));
    TEST_ASSERT_EQUAL(UI_SCREEN_NO_CLOCK, ui_render(&ctx, &ac, at(ms + 100, T0)).screen);
}

TEST_CASE("the panel is dark until someone presses a key", "[ui]")
{
    ui_fixture();
    TEST_ASSERT_FALSE(ui_render(&ctx, &ac, at(0, T0)).panel_on);

    /* The first press is the first character, not a wake-up that gets
     * thrown away: the bar appearing is the confirmation. */
    TEST_ASSERT_EQUAL(UI_ACTION_NONE, ui_on_key(&ctx, &ac, at(100, T0)));
    ui_render_t r = ui_render(&ctx, &ac, at(100, T0));
    TEST_ASSERT_EQUAL(UI_SCREEN_ENTRY, r.screen);
    TEST_ASSERT_TRUE(r.panel_on);

    /* Abandoned entry: dark again. */
    ui_tick(&ctx, &ac, at(100 + UI_ENTRY_TIMEOUT_MS, T0));
    TEST_ASSERT_FALSE(ui_render(&ctx, &ac, at(100 + UI_ENTRY_TIMEOUT_MS, T0)).panel_on);
}

TEST_CASE("every interaction ends with the panel dark", "[ui]")
{
    ui_fixture();

    ac_result_t g = ac_evaluate(&ac, good_hash, T0, NULL);
    ui_on_result(&ctx, &ac, g, at(0, T0));
    TEST_ASSERT_TRUE(ui_render(&ctx, &ac, at(0, T0)).panel_on);
    ui_tick(&ctx, &ac, at(UI_GRANTED_MS, T0));
    TEST_ASSERT_FALSE(ui_render(&ctx, &ac, at(UI_GRANTED_MS, T0)).panel_on);

    fail_n(1, T0);
    TEST_ASSERT_TRUE(ui_render(&ctx, &ac, at(0, T0)).panel_on);
    ui_tick(&ctx, &ac, at(UI_DENIED_MS, T0));
    TEST_ASSERT_FALSE(ui_render(&ctx, &ac, at(UI_DENIED_MS, T0)).panel_on);
}

TEST_CASE("a lockout keeps the panel lit only while it runs", "[ui]")
{
    ui_fixture();
    fail_n(MAXF, T0);
    TEST_ASSERT_TRUE(ui_render(&ctx, &ac, at(0, T0)).panel_on);
    TEST_ASSERT_TRUE(ui_render(&ctx, &ac, at(0, T0 + LOCK_S - 1)).panel_on);

    int64_t after = T0 + LOCK_S + 1;
    ac_tick(&ac, after);
    ui_tick(&ctx, &ac, at(0, after));
    TEST_ASSERT_FALSE(ui_render(&ctx, &ac, at(0, after)).panel_on);
}
