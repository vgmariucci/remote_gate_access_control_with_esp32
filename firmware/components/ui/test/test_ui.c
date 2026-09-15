/*
 * Unity tests for the guest-facing state machine.
 *
 * The security-relevant assertions are the ones about what does NOT
 * happen: the buffer length never reaches the renderer, an expired code
 * does not burn an attempt, and the lockout survives a restart.
 */
#include "ui.h"
#include "unity.h"

static ui_ctx_t ctx;

static void press_n(uint32_t *t, int n)
{
    for (int i = 0; i < n; i++) {
        ui_on_key(&ctx, *t);
        *t += 400;
    }
}

/* Not setUp(): three suites link into one host binary, so a global
 * setUp() would be defined three times. Each case calls this itself. */
static void ui_fixture(void)
{
    ui_init(&ctx, 0, 0, 0);
    ui_set_clock_trusted(&ctx, true);
}

TEST_CASE("boot without trusted time refuses everything", "[ui]")
{
    ui_fixture();
    ui_init(&ctx, 0, 0, 0);
    TEST_ASSERT_EQUAL(UI_SCREEN_NO_CLOCK, ui_render(&ctx, 0).screen);
    TEST_ASSERT_EQUAL(UI_ACTION_NONE, ui_on_key(&ctx, 10));
    TEST_ASSERT_EQUAL(UI_SCREEN_NO_CLOCK, ui_render(&ctx, 20).screen);
}

TEST_CASE("the render payload never carries the entry length", "[ui]")
{
    ui_fixture();
    uint32_t t = 0;
    press_n(&t, 4);
    ui_render_t r = ui_render(&ctx, t);
    TEST_ASSERT_EQUAL(UI_SCREEN_ENTRY, r.screen);
    /* Only a countdown and an attempt count are exposed. If a future
     * change adds a length field here, this test should be the thing
     * that stops it. */
    TEST_ASSERT_EQUAL_UINT8(0, r.attempts_used);
    TEST_ASSERT_EQUAL_UINT32(0, r.seconds_remaining);
}

TEST_CASE("each keypress refills the countdown bar", "[ui]")
{
    ui_fixture();
    uint32_t t = 0;
    ui_on_key(&ctx, t);
    TEST_ASSERT_EQUAL_UINT16(1000, ui_render(&ctx, t).progress_permille);

    t += 5000;
    TEST_ASSERT_EQUAL_UINT16(500, ui_render(&ctx, t).progress_permille);

    ui_on_key(&ctx, t); /* the visible reset is the press feedback */
    TEST_ASSERT_EQUAL_UINT16(1000, ui_render(&ctx, t).progress_permille);
}

TEST_CASE("the ninth key submits", "[ui]")
{
    ui_fixture();
    uint32_t t = 0;
    for (int i = 0; i < UI_CODE_LEN - 1; i++) {
        TEST_ASSERT_EQUAL(UI_ACTION_NONE, ui_on_key(&ctx, t));
        t += 300;
    }
    TEST_ASSERT_EQUAL(UI_ACTION_SUBMIT, ui_on_key(&ctx, t));
}

TEST_CASE("ten seconds of silence abandons the entry", "[ui]")
{
    ui_fixture();
    uint32_t t = 0;
    press_n(&t, 3);
    TEST_ASSERT_EQUAL(UI_SCREEN_ENTRY, ui_render(&ctx, t).screen);

    t += UI_ENTRY_TIMEOUT_MS;
    TEST_ASSERT_EQUAL(UI_ACTION_CLEAR_BUFFER, ui_tick(&ctx, t));
    TEST_ASSERT_EQUAL(UI_SCREEN_IDLE, ui_render(&ctx, t).screen);
    /* An abandoned entry is not a wrong guess. */
    TEST_ASSERT_EQUAL_UINT8(0, ui_render(&ctx, t).attempts_used);
}

TEST_CASE("long press clears immediately", "[ui]")
{
    ui_fixture();
    uint32_t t = 0;
    press_n(&t, 5);
    TEST_ASSERT_EQUAL(UI_ACTION_CLEAR_BUFFER, ui_on_long_press(&ctx, t));
    TEST_ASSERT_EQUAL(UI_SCREEN_IDLE, ui_render(&ctx, t).screen);

    /* And the next nine presses submit, proving the counter really reset. */
    for (int i = 0; i < UI_CODE_LEN - 1; i++) {
        TEST_ASSERT_EQUAL(UI_ACTION_NONE, ui_on_key(&ctx, t));
        t += 100;
    }
    TEST_ASSERT_EQUAL(UI_ACTION_SUBMIT, ui_on_key(&ctx, t));
}

TEST_CASE("granted shows OK for three seconds then returns to idle", "[ui]")
{
    ui_fixture();
    uint32_t t = 1000;
    ui_on_result(&ctx, AC_GRANTED, t);
    TEST_ASSERT_EQUAL(UI_SCREEN_GRANTED, ui_render(&ctx, t).screen);

    ui_tick(&ctx, t + UI_GRANTED_MS - 1);
    TEST_ASSERT_EQUAL(UI_SCREEN_GRANTED, ui_render(&ctx, t + UI_GRANTED_MS - 1).screen);

    ui_tick(&ctx, t + UI_GRANTED_MS);
    TEST_ASSERT_EQUAL(UI_SCREEN_IDLE, ui_render(&ctx, t + UI_GRANTED_MS).screen);
}

TEST_CASE("wrong codes count up to five then lock out", "[ui]")
{
    ui_fixture();
    uint32_t t = 0;
    for (int i = 1; i <= 4; i++) {
        ui_on_result(&ctx, AC_DENIED_UNKNOWN, t);
        ui_render_t r = ui_render(&ctx, t);
        TEST_ASSERT_EQUAL(UI_SCREEN_DENIED, r.screen);
        TEST_ASSERT_EQUAL_UINT8(i, r.attempts_used);
        t += UI_DENIED_MS;
        ui_tick(&ctx, t);
    }

    ui_on_result(&ctx, AC_DENIED_UNKNOWN, t);
    ui_render_t r = ui_render(&ctx, t);
    TEST_ASSERT_EQUAL(UI_SCREEN_LOCKOUT, r.screen);
    TEST_ASSERT_EQUAL_UINT8(UI_MAX_ATTEMPTS, r.attempts_used);
    TEST_ASSERT_EQUAL_UINT32(300, r.seconds_remaining);
    TEST_ASSERT_TRUE(ui_is_locked_out(&ctx, t));
}

TEST_CASE("the keypad is inert during lockout", "[ui]")
{
    ui_fixture();
    uint32_t t = 0;
    uint32_t locked_at = 0;
    for (int i = 0; i < UI_MAX_ATTEMPTS; i++) {
        locked_at = t; /* the fifth denial is what arms the lockout */
        ui_on_result(&ctx, AC_DENIED_UNKNOWN, t);
        t += UI_DENIED_MS;
        ui_tick(&ctx, t);
    }
    TEST_ASSERT_EQUAL(UI_ACTION_NONE, ui_on_key(&ctx, t + 1000));
    TEST_ASSERT_EQUAL(UI_SCREEN_LOCKOUT, ui_render(&ctx, t + 1000).screen);

    /* The bar measures from when the lockout was armed, not from the
     * last tick. Halfway through, it is half full. */
    TEST_ASSERT_EQUAL_UINT16(500,
                             ui_render(&ctx, locked_at + UI_LOCKOUT_MS / 2).progress_permille);
    TEST_ASSERT_EQUAL_UINT16(0, ui_render(&ctx, locked_at + UI_LOCKOUT_MS).progress_permille);
}

TEST_CASE("lockout releases and clears the counter", "[ui]")
{
    ui_fixture();
    uint32_t t = 0;
    for (int i = 0; i < UI_MAX_ATTEMPTS; i++) {
        ui_on_result(&ctx, AC_DENIED_UNKNOWN, t);
        t += UI_DENIED_MS;
        ui_tick(&ctx, t);
    }
    uint32_t after = t + UI_LOCKOUT_MS + 1;
    ui_tick(&ctx, after);
    TEST_ASSERT_FALSE(ui_is_locked_out(&ctx, after));
    TEST_ASSERT_EQUAL(UI_SCREEN_IDLE, ui_render(&ctx, after).screen);
    TEST_ASSERT_EQUAL_UINT8(0, ui_render(&ctx, after).attempts_used);
}

TEST_CASE("a lockout restored from RTC SRAM still holds", "[ui]")
{
    ui_fixture();
    /* Power was cut 100 s into a 300 s lockout. The DS3232 kept the
     * counter, so the reboot must not hand the attacker a clean slate. */
    ui_init(&ctx, 5, 200000, 0);
    ui_set_clock_trusted(&ctx, true);
    TEST_ASSERT_TRUE(ui_is_locked_out(&ctx, 0));
    TEST_ASSERT_EQUAL(UI_ACTION_NONE, ui_on_key(&ctx, 100));
    TEST_ASSERT_EQUAL_UINT32(200, ui_render(&ctx, 0).seconds_remaining);
}

TEST_CASE("a genuine code outside its window costs no attempt", "[ui]")
{
    ui_fixture();
    uint32_t t = 0;
    ui_on_result(&ctx, AC_DENIED_NOT_YET, t);
    TEST_ASSERT_EQUAL(UI_SCREEN_NOT_YET, ui_render(&ctx, t).screen);
    TEST_ASSERT_EQUAL_UINT8(0, ui_render(&ctx, t).attempts_used);

    t += UI_INFO_MS;
    ui_tick(&ctx, t);

    ui_on_result(&ctx, AC_DENIED_EXPIRED, t);
    TEST_ASSERT_EQUAL(UI_SCREEN_EXPIRED, ui_render(&ctx, t).screen);
    TEST_ASSERT_EQUAL_UINT8(0, ui_render(&ctx, t).attempts_used);

    /* Ten early arrivals must not lock the real guest out. */
    for (int i = 0; i < 10; i++) {
        ui_on_result(&ctx, AC_DENIED_NOT_YET, t);
        t += UI_INFO_MS;
        ui_tick(&ctx, t);
    }
    TEST_ASSERT_FALSE(ui_is_locked_out(&ctx, t));
}

TEST_CASE("a success wipes the accumulated attempts", "[ui]")
{
    ui_fixture();
    uint32_t t = 0;
    ui_on_result(&ctx, AC_DENIED_UNKNOWN, t);
    t += UI_DENIED_MS;
    ui_tick(&ctx, t);
    ui_on_result(&ctx, AC_DENIED_UNKNOWN, t);
    t += UI_DENIED_MS;
    ui_tick(&ctx, t);
    TEST_ASSERT_EQUAL_UINT8(2, ui_render(&ctx, t).attempts_used);

    ui_on_result(&ctx, AC_GRANTED, t);
    TEST_ASSERT_EQUAL_UINT8(0, ui_render(&ctx, t).attempts_used);
}

TEST_CASE("losing trusted time mid-entry fails closed", "[ui]")
{
    ui_fixture();
    uint32_t t = 0;
    press_n(&t, 6);
    ui_set_clock_trusted(&ctx, false);
    TEST_ASSERT_EQUAL(UI_SCREEN_NO_CLOCK, ui_render(&ctx, t).screen);
    TEST_ASSERT_EQUAL(UI_ACTION_NONE, ui_on_key(&ctx, t + 100));
}