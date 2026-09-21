/*
 * Unity tests for lock_logic.
 *
 * The assertions that matter are the ones about the coil NOT being
 * energised: a burnt solenoid is a service call, and the failure modes
 * that cause it (a stalled loop, two grants in a row, a bad config) are
 * all timing bugs that are miserable to reproduce on hardware.
 */
#include "lock_logic.h"
#include "unity.h"

static lock_logic_t lk;

/* No global setUp(): several suites link into one host binary. */
static void lock_fixture(void)
{
    lock_logic_init(&lk, LOCK_DEFAULT_PULSE_MS);
}

TEST_CASE("the coil is de-energised at init", "[lock]")
{
    lock_fixture();
    TEST_ASSERT_EQUAL(LOCK_IDLE, lk.state);
    TEST_ASSERT_FALSE(lock_logic_output(&lk, 0));
    TEST_ASSERT_FALSE(lock_logic_output(&lk, 100000));
}

TEST_CASE("a request energises the coil and it drops at the pulse length", "[lock]")
{
    lock_fixture();
    TEST_ASSERT_TRUE(lock_logic_request(&lk, 1000));
    TEST_ASSERT_TRUE(lock_logic_output(&lk, 1000));
    TEST_ASSERT_TRUE(lock_logic_output(&lk, 1000 + LOCK_DEFAULT_PULSE_MS - 1));
    TEST_ASSERT_FALSE(lock_logic_output(&lk, 1000 + LOCK_DEFAULT_PULSE_MS));
    TEST_ASSERT_EQUAL_UINT32(1, lk.pulses_total);
}

TEST_CASE("a stalled main loop still drops the coil", "[lock]")
{
    lock_fixture();
    lock_logic_request(&lk, 0);
    /* Nothing ticks for ten seconds: a long flash write, a blocked
     * task, a debugger breakpoint. The output must read false anyway. */
    TEST_ASSERT_FALSE(lock_logic_output(&lk, 10000));
    lock_logic_tick(&lk, 10000);
    TEST_ASSERT_FALSE(lock_logic_output(&lk, 10000));
}

TEST_CASE("a request during a pulse is refused, never merged", "[lock]")
{
    lock_fixture();
    TEST_ASSERT_TRUE(lock_logic_request(&lk, 0));
    TEST_ASSERT_FALSE(lock_logic_request(&lk, 400));
    TEST_ASSERT_EQUAL_UINT32(1, lk.requests_refused);

    /* The refusal must not have extended the pulse. */
    TEST_ASSERT_FALSE(lock_logic_output(&lk, LOCK_DEFAULT_PULSE_MS));
    TEST_ASSERT_EQUAL_UINT32(1, lk.pulses_total);
}

TEST_CASE("the cooldown refuses requests, then lifts", "[lock]")
{
    lock_fixture();
    lock_logic_request(&lk, 0);
    uint32_t ends = LOCK_DEFAULT_PULSE_MS;

    lock_logic_tick(&lk, ends);
    TEST_ASSERT_EQUAL(LOCK_COOLDOWN, lk.state);
    TEST_ASSERT_FALSE(lock_logic_request(&lk, ends + 1));
    TEST_ASSERT_FALSE(lock_logic_request(&lk, ends + LOCK_COOLDOWN_MS - 1));

    TEST_ASSERT_TRUE(lock_logic_request(&lk, ends + LOCK_COOLDOWN_MS));
    TEST_ASSERT_EQUAL_UINT32(2, lk.pulses_total);
}

TEST_CASE("a refused request is dropped, not queued", "[lock]")
{
    lock_fixture();
    lock_logic_request(&lk, 0);
    for (int i = 1; i <= 20; i++) {
        lock_logic_request(&lk, (uint32_t)(i * 100));
    }
    /* Twenty refusals must not produce twenty pulses once the cooldown
     * lifts, with nobody at the door. */
    uint32_t after = LOCK_DEFAULT_PULSE_MS + LOCK_COOLDOWN_MS + 1;
    lock_logic_tick(&lk, after);
    TEST_ASSERT_EQUAL(LOCK_IDLE, lk.state);
    TEST_ASSERT_FALSE(lock_logic_output(&lk, after));
    TEST_ASSERT_EQUAL_UINT32(1, lk.pulses_total);
}

TEST_CASE("a pulse longer than the ceiling is clamped", "[lock]")
{
    lock_logic_init(&lk, 30000); /* a typo, or an electromagnet config */
    TEST_ASSERT_EQUAL_UINT32(LOCK_MAX_PULSE_MS, lk.pulse_ms);

    lock_logic_request(&lk, 0);
    TEST_ASSERT_FALSE(lock_logic_output(&lk, LOCK_MAX_PULSE_MS));
}

TEST_CASE("a zero pulse falls back to the default, not to nothing", "[lock]")
{
    lock_logic_init(&lk, 0);
    TEST_ASSERT_EQUAL_UINT32(LOCK_DEFAULT_PULSE_MS, lk.pulse_ms);
    lock_logic_request(&lk, 0);
    TEST_ASSERT_TRUE(lock_logic_output(&lk, 10));
}

TEST_CASE("duty cycle stays bounded under continuous requests", "[lock]")
{
    lock_fixture();
    uint32_t energised_ms = 0;
    /* Hammer it for a simulated minute at 10 ms resolution. */
    for (uint32_t t = 0; t < 60000; t += 10) {
        lock_logic_request(&lk, t);
        lock_logic_tick(&lk, t);
        if (lock_logic_output(&lk, t)) {
            energised_ms += 10;
        }
    }
    /* pulse / (pulse + cooldown) = 800/3800 = 21%. Assert a comfortable
     * ceiling rather than the exact figure. */
    TEST_ASSERT_TRUE(energised_ms < 60000 / 4);
}

TEST_CASE("the millisecond counter wrapping does not stick the coil on", "[lock]")
{
    lock_fixture();
    uint32_t near_wrap = 0xFFFFFF00u;
    TEST_ASSERT_TRUE(lock_logic_request(&lk, near_wrap));
    TEST_ASSERT_TRUE(lock_logic_output(&lk, near_wrap + 100));
    /* esp_timer gives 64-bit microseconds so this cannot happen in
     * practice, but the arithmetic must be unsigned-safe regardless. */
    TEST_ASSERT_FALSE(lock_logic_output(&lk, near_wrap + LOCK_DEFAULT_PULSE_MS));
}
