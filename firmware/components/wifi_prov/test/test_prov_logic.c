/*
 * Unity tests for prov_logic.
 *
 * The assertions that matter are about the AP NOT opening: nothing but
 * a deliberate hold of the button gets in, and the session closes on
 * its own whether or not anyone tells it to.
 */
#include <string.h>

#include "prov_logic.h"
#include "unity.h"

static prov_ctx_t ctx;

static void fixture(void)
{
    prov_init(&ctx);
}

/* Holds the button from `from_ms` for `hold_ms`, scanning every 50 ms
 * the way the main loop does. Returns the action seen. */
static prov_action_t hold(uint32_t from_ms, uint32_t hold_ms)
{
    prov_action_t seen = PROV_ACTION_NONE;
    for (uint32_t t = from_ms; t <= from_ms + hold_ms; t += 50) {
        prov_action_t a = prov_button(&ctx, true, t);
        if (a != PROV_ACTION_NONE) {
            seen = a;
        }
    }
    prov_button(&ctx, false, from_ms + hold_ms + 50);
    return seen;
}

/* ------------------------------------------------------- credentials */

TEST_CASE("a usable network name is accepted", "[prov]")
{
    TEST_ASSERT_EQUAL(PROV_OK, prov_check_ssid("casa-wifi"));
    TEST_ASSERT_EQUAL(PROV_OK, prov_check_ssid("A"));
    TEST_ASSERT_EQUAL(PROV_OK, prov_check_ssid("12345678901234567890123456789012")); /* 32 */
}

TEST_CASE("a broken network name is rejected with a reason", "[prov]")
{
    TEST_ASSERT_EQUAL(PROV_ERR_SSID_EMPTY, prov_check_ssid(""));
    TEST_ASSERT_EQUAL(PROV_ERR_SSID_EMPTY, prov_check_ssid(NULL));
    TEST_ASSERT_EQUAL(PROV_ERR_SSID_LONG,
                      prov_check_ssid("123456789012345678901234567890123")); /* 33 */
    TEST_ASSERT_EQUAL(PROV_ERR_SSID_CONTROL, prov_check_ssid("casa\twifi"));
    /* Every failure has text the portal can show. */
    TEST_ASSERT_TRUE(strlen(prov_result_text(PROV_ERR_SSID_LONG)) > 0);
}

TEST_CASE("WPA2 passphrase lengths are enforced", "[prov]")
{
    TEST_ASSERT_EQUAL(PROV_OK, prov_check_psk("12345678", false));
    TEST_ASSERT_EQUAL(PROV_ERR_PSK_SHORT, prov_check_psk("1234567", false));
    TEST_ASSERT_EQUAL(PROV_ERR_PSK_SHORT, prov_check_psk("", false));

    char long_psk[80];
    memset(long_psk, 'a', sizeof(long_psk));
    long_psk[63] = '\0';
    TEST_ASSERT_EQUAL(PROV_OK, prov_check_psk(long_psk, false));
    long_psk[64] = '\0';
    memset(long_psk, 'z', 64); /* 64 chars, not hex: too long, not a PSK */
    TEST_ASSERT_EQUAL(PROV_ERR_PSK_LONG, prov_check_psk(long_psk, false));
}

TEST_CASE("a raw 64-character hex PSK is accepted", "[prov]")
{
    char psk[65];
    memset(psk, 'a', 64);
    psk[64] = '\0';
    TEST_ASSERT_EQUAL(PROV_OK, prov_check_psk(psk, false));
}

TEST_CASE("an open network takes no passphrase", "[prov]")
{
    TEST_ASSERT_EQUAL(PROV_OK, prov_check_psk("", true));
    /* Typing one is a mistake worth reporting, not silently dropping. */
    TEST_ASSERT_TRUE(prov_check_psk("hunter2", true) != PROV_OK);
}

TEST_CASE("WEP is refused outright", "[prov]")
{
    TEST_ASSERT_EQUAL(PROV_ERR_AUTH_WEP, prov_check_auth(PROV_AUTH_WEP));
    TEST_ASSERT_EQUAL(PROV_OK, prov_check_auth(PROV_AUTH_WPA2_PSK));
    TEST_ASSERT_EQUAL(PROV_OK, prov_check_auth(PROV_AUTH_WPA3_PSK));
    TEST_ASSERT_EQUAL(PROV_OK, prov_check_auth(PROV_AUTH_OPEN));
}

TEST_CASE("weak but usable security is flagged, not blocked", "[prov]")
{
    TEST_ASSERT_TRUE(prov_auth_is_weak(PROV_AUTH_OPEN));
    TEST_ASSERT_TRUE(prov_auth_is_weak(PROV_AUTH_WPA_PSK));
    TEST_ASSERT_FALSE(prov_auth_is_weak(PROV_AUTH_WPA2_PSK));
    TEST_ASSERT_FALSE(prov_auth_is_weak(PROV_AUTH_WPA2_WPA3_PSK));
}

/* ----------------------------------------------------------- session */

TEST_CASE("the AP is closed until the button is held", "[prov]")
{
    fixture();
    TEST_ASSERT_FALSE(prov_is_open(&ctx));
    /* Ticking forever never opens it: there is no path from anything
     * else - a failed Wi-Fi join included - into configuration mode. */
    for (uint32_t t = 0; t < 10u * 60u * 1000u; t += 1000) {
        TEST_ASSERT_EQUAL(PROV_ACTION_NONE, prov_tick(&ctx, t));
    }
    TEST_ASSERT_FALSE(prov_is_open(&ctx));
}

TEST_CASE("a brief press does nothing", "[prov]")
{
    fixture();
    /* Closing the enclosure lid must not arm the portal. */
    TEST_ASSERT_EQUAL(PROV_ACTION_NONE, hold(0, PROV_HOLD_MS - 500));
    TEST_ASSERT_FALSE(prov_is_open(&ctx));
}

TEST_CASE("a five-second hold opens the AP", "[prov]")
{
    fixture();
    TEST_ASSERT_EQUAL(PROV_ACTION_OPEN_AP, hold(0, PROV_HOLD_MS));
    TEST_ASSERT_TRUE(prov_is_open(&ctx));
    TEST_ASSERT_FALSE(prov_is_authenticated(&ctx));
}

TEST_CASE("holding longer does not open it twice", "[prov]")
{
    fixture();
    int opens = 0;
    for (uint32_t t = 0; t <= 30000; t += 50) {
        if (prov_button(&ctx, true, t) == PROV_ACTION_OPEN_AP) {
            opens++;
        }
    }
    TEST_ASSERT_EQUAL_INT(1, opens);
}

TEST_CASE("the session closes after five minutes", "[prov]")
{
    fixture();
    hold(0, PROV_HOLD_MS);
    uint32_t opened = PROV_HOLD_MS;

    TEST_ASSERT_EQUAL(PROV_ACTION_NONE, prov_tick(&ctx, opened + PROV_SESSION_MS - 1000));
    TEST_ASSERT_TRUE(prov_is_open(&ctx));

    TEST_ASSERT_EQUAL(PROV_ACTION_CLOSE_AP, prov_tick(&ctx, opened + PROV_SESSION_MS));
    TEST_ASSERT_FALSE(prov_is_open(&ctx));
    /* Reported once, so the caller tears down once. */
    TEST_ASSERT_EQUAL(PROV_ACTION_NONE, prov_tick(&ctx, opened + PROV_SESSION_MS + 5000));
}

TEST_CASE("activity does not extend the session", "[prov]")
{
    fixture();
    hold(0, PROV_HOLD_MS);
    uint32_t opened = PROV_HOLD_MS;
    /* A busy admin does not keep the radio up indefinitely. */
    for (uint32_t t = opened; t < opened + PROV_SESSION_MS; t += 1000) {
        prov_login(&ctx, true);
        prov_tick(&ctx, t);
    }
    TEST_ASSERT_EQUAL(PROV_ACTION_CLOSE_AP, prov_tick(&ctx, opened + PROV_SESSION_MS));
}

TEST_CASE("a session that ends while the button is held does not reopen", "[prov]")
{
    fixture();
    /* Someone leans on the button for ten minutes. */
    uint32_t opened = 0;
    int opens = 0;
    for (uint32_t t = 0; t < 10u * 60u * 1000u; t += 50) {
        if (prov_button(&ctx, true, t) == PROV_ACTION_OPEN_AP) {
            opens++;
            opened = t;
        }
        prov_tick(&ctx, t);
    }
    TEST_ASSERT_EQUAL_INT(1, opens);
    TEST_ASSERT_FALSE(prov_is_open(&ctx));
    TEST_ASSERT_TRUE(opened < PROV_HOLD_MS + 100);
}

TEST_CASE("releasing and holding again opens a new session", "[prov]")
{
    fixture();
    TEST_ASSERT_EQUAL(PROV_ACTION_OPEN_AP, hold(0, PROV_HOLD_MS));
    prov_close(&ctx);
    TEST_ASSERT_EQUAL(PROV_ACTION_OPEN_AP, hold(60000, PROV_HOLD_MS));
    TEST_ASSERT_TRUE(prov_is_open(&ctx));
}

TEST_CASE("an explicit close ends the session once", "[prov]")
{
    fixture();
    hold(0, PROV_HOLD_MS);
    TEST_ASSERT_EQUAL(PROV_ACTION_CLOSE_AP, prov_close(&ctx));
    TEST_ASSERT_EQUAL(PROV_ACTION_NONE, prov_close(&ctx));
    TEST_ASSERT_FALSE(prov_is_open(&ctx));
}

TEST_CASE("five wrong logins tear the session down", "[prov]")
{
    fixture();
    hold(0, PROV_HOLD_MS);
    for (int i = 0; i < PROV_MAX_LOGIN_FAILURES - 1; i++) {
        TEST_ASSERT_FALSE(prov_login(&ctx, false));
        TEST_ASSERT_TRUE(prov_is_open(&ctx));
    }
    TEST_ASSERT_FALSE(prov_login(&ctx, false));
    /* Physical access to the button is needed to try again. */
    TEST_ASSERT_FALSE(prov_is_open(&ctx));
}

TEST_CASE("a correct login clears the failure count", "[prov]")
{
    fixture();
    hold(0, PROV_HOLD_MS);
    prov_login(&ctx, false);
    prov_login(&ctx, false);
    TEST_ASSERT_TRUE(prov_login(&ctx, true));
    TEST_ASSERT_TRUE(prov_is_authenticated(&ctx));

    for (int i = 0; i < PROV_MAX_LOGIN_FAILURES - 1; i++) {
        prov_login(&ctx, false);
    }
    TEST_ASSERT_TRUE(prov_is_open(&ctx));
}

TEST_CASE("authentication does not survive the session", "[prov]")
{
    fixture();
    hold(0, PROV_HOLD_MS);
    prov_login(&ctx, true);
    TEST_ASSERT_TRUE(prov_is_authenticated(&ctx));

    prov_close(&ctx);
    TEST_ASSERT_FALSE(prov_is_authenticated(&ctx));
    hold(60000, PROV_HOLD_MS);
    TEST_ASSERT_FALSE(prov_is_authenticated(&ctx));
}

TEST_CASE("logging in is impossible while the AP is closed", "[prov]")
{
    fixture();
    TEST_ASSERT_FALSE(prov_login(&ctx, true));
    TEST_ASSERT_FALSE(prov_is_authenticated(&ctx));
}

TEST_CASE("the countdown falls to zero and stops", "[prov]")
{
    fixture();
    hold(0, PROV_HOLD_MS);
    uint32_t opened = PROV_HOLD_MS;
    TEST_ASSERT_EQUAL_UINT32(300, prov_seconds_left(&ctx, opened));
    TEST_ASSERT_EQUAL_UINT32(240, prov_seconds_left(&ctx, opened + 60000));
    TEST_ASSERT_EQUAL_UINT32(0, prov_seconds_left(&ctx, opened + PROV_SESSION_MS));
    prov_close(&ctx);
    TEST_ASSERT_EQUAL_UINT32(0, prov_seconds_left(&ctx, opened));
}
