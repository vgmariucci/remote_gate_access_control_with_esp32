/*
 * Unity tests for keypad_logic. Runs on the host in CI and on the target
 * with `idf.py -T keypad test`.
 *
 * Every test drives the logic with a synthetic scan sequence, which is
 * the whole point of keeping it free of GPIO: bounce and long-press are
 * timing bugs, and timing bugs are miserable to reproduce on hardware.
 */
#include "keypad_logic.h"
#include "unity.h"

#define BIT_FOR(idx) ((uint16_t)(1u << (idx)))
#define KEY_1 BIT_FOR(0)
#define KEY_5 BIT_FOR(5)
#define KEY_A BIT_FOR(3)

static kp_logic_t st;
static kp_event_t ev[KP_MAX_EVENTS];

static size_t feed(uint16_t raw, uint32_t now_ms)
{
    return kp_logic_update(&st, raw, now_ms, ev, KP_MAX_EVENTS);
}

/* See the note in test_ui.c: no global setUp() in a multi-suite binary. */
static void kp_fixture(void)
{
    kp_logic_init(&st);
}

TEST_CASE("keymap covers all sixteen positions", "[keypad]")
{
    kp_fixture();
    const char expected[] = "123A456B789C*0#D";
    for (int i = 0; i < KP_KEYS; i++) {
        TEST_ASSERT_EQUAL_CHAR(expected[i], kp_index_to_char(i));
    }
    TEST_ASSERT_EQUAL_CHAR(0, kp_index_to_char(-1));
    TEST_ASSERT_EQUAL_CHAR(0, kp_index_to_char(KP_KEYS));
}

TEST_CASE("a press shorter than the debounce window is ignored", "[keypad]")
{
    kp_fixture();
    TEST_ASSERT_EQUAL_UINT(0, feed(KEY_5, 0));
    TEST_ASSERT_EQUAL_UINT(0, feed(KEY_5, 10)); /* still under 15 ms */
    TEST_ASSERT_EQUAL_UINT(0, feed(0, 12));
    TEST_ASSERT_EQUAL_UINT(0, feed(0, 40));
}

TEST_CASE("a stable press emits exactly one event", "[keypad]")
{
    kp_fixture();
    TEST_ASSERT_EQUAL_UINT(0, feed(KEY_5, 0));
    TEST_ASSERT_EQUAL_UINT(1, feed(KEY_5, KP_DEBOUNCE_MS));
    TEST_ASSERT_EQUAL(KP_EV_PRESS, ev[0].type);
    TEST_ASSERT_EQUAL_CHAR('5', ev[0].key); /* index 5 -> '5' */

    /* Holding it must not repeat before the long-press threshold. */
    TEST_ASSERT_EQUAL_UINT(0, feed(KEY_5, KP_DEBOUNCE_MS + 5));
    TEST_ASSERT_EQUAL_UINT(0, feed(KEY_5, KP_DEBOUNCE_MS + 100));
}

TEST_CASE("contact bounce produces one event, not several", "[keypad]")
{
    kp_fixture();
    uint32_t t = 0;
    const uint16_t chatter[] = {KEY_1, 0, KEY_1, 0, KEY_1};
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_UINT(0, feed(chatter[i], t));
        t += 3;
    }
    size_t n = 0;
    for (int i = 0; i < 10; i++) {
        n += feed(KEY_1, t);
        t += 3;
    }
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_CHAR('1', ev[0].key);
}

TEST_CASE("long press fires once and only once", "[keypad]")
{
    kp_fixture();
    TEST_ASSERT_EQUAL_UINT(0, feed(KEY_A, 0));
    TEST_ASSERT_EQUAL_UINT(1, feed(KEY_A, KP_DEBOUNCE_MS));
    TEST_ASSERT_EQUAL(KP_EV_PRESS, ev[0].type);

    TEST_ASSERT_EQUAL_UINT(0, feed(KEY_A, KP_DEBOUNCE_MS + 1000));

    size_t n = feed(KEY_A, KP_DEBOUNCE_MS + KP_LONGPRESS_MS);
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL(KP_EV_LONG_PRESS, ev[0].type);
    TEST_ASSERT_EQUAL_CHAR('A', ev[0].key);

    /* Keep holding: no second long press. */
    TEST_ASSERT_EQUAL_UINT(0, feed(KEY_A, KP_DEBOUNCE_MS + KP_LONGPRESS_MS + 500));
    TEST_ASSERT_EQUAL_UINT(0, feed(KEY_A, KP_DEBOUNCE_MS + KP_LONGPRESS_MS + 5000));
}

TEST_CASE("two keys at once are rejected until release", "[keypad]")
{
    kp_fixture();
    uint32_t t = 0;
    TEST_ASSERT_EQUAL_UINT(0, feed(KEY_1 | KEY_5, t));
    t += KP_DEBOUNCE_MS;
    TEST_ASSERT_EQUAL_UINT(0, feed(KEY_1 | KEY_5, t));

    /* Lifting one finger must not be read as a press of the other. */
    t += 20;
    TEST_ASSERT_EQUAL_UINT(0, feed(KEY_5, t));
    t += KP_DEBOUNCE_MS;
    TEST_ASSERT_EQUAL_UINT(0, feed(KEY_5, t));

    /* Full release clears the block. */
    t += 20;
    feed(0, t);
    t += KP_DEBOUNCE_MS;
    feed(0, t);

    t += 20;
    TEST_ASSERT_EQUAL_UINT(0, feed(KEY_5, t));
    t += KP_DEBOUNCE_MS;
    TEST_ASSERT_EQUAL_UINT(1, feed(KEY_5, t));
    TEST_ASSERT_EQUAL_CHAR('5', ev[0].key);
}

TEST_CASE("nine sequential presses yield nine events", "[keypad]")
{
    kp_fixture();
    const int indices[9] = {0, 1, 2, 4, 5, 6, 3, 7, 12}; /* 123456AB* */
    uint32_t t = 0;
    int seen = 0;

    for (int i = 0; i < 9; i++) {
        uint16_t k = BIT_FOR(indices[i]);
        feed(k, t);
        t += KP_DEBOUNCE_MS;
        seen += (int)feed(k, t);
        t += 30;
        feed(0, t);
        t += KP_DEBOUNCE_MS;
        feed(0, t);
        t += 30;
    }
    TEST_ASSERT_EQUAL_INT(9, seen);
}