/*
 * Unity tests for net_retry.
 *
 * The property that matters is that the gate keeps trying forever
 * without hammering the router, and that it comes back quickly when
 * the router does.
 */
#include "net_retry.h"
#include "unity.h"

static net_retry_t r;

TEST_CASE("the first attempt waits for nothing", "[net]")
{
    net_retry_init(&r, 1000);
    TEST_ASSERT_TRUE(net_retry_due(&r, 1000));
}

TEST_CASE("the backoff doubles and then holds at the ceiling", "[net]")
{
    net_retry_init(&r, 0);
    const uint32_t expected[] = {1000, 2000, 4000, 8000, 16000, 32000, 60000, 60000, 60000};

    uint32_t now = 0;
    for (int i = 0; i < 9; i++) {
        TEST_ASSERT_EQUAL_UINT32(expected[i], net_retry_delay_ms(&r));
        net_retry_failed(&r, now);
        now += expected[i];
    }
    /* Never past the cap, however long the outage. */
    for (int i = 0; i < 100; i++) {
        net_retry_failed(&r, now);
        now += NET_RETRY_MAX_MS;
    }
    TEST_ASSERT_EQUAL_UINT32(NET_RETRY_MAX_MS, net_retry_delay_ms(&r));
}

TEST_CASE("a failure is not retried before its delay", "[net]")
{
    net_retry_init(&r, 0);
    net_retry_failed(&r, 0); /* next try at 1000 */
    TEST_ASSERT_FALSE(net_retry_due(&r, 999));
    TEST_ASSERT_TRUE(net_retry_due(&r, 1000));
    TEST_ASSERT_TRUE(net_retry_due(&r, 5000));
}

TEST_CASE("a connected link is never retried", "[net]")
{
    net_retry_init(&r, 0);
    net_retry_connected(&r);
    for (uint32_t t = 0; t < 600000; t += 1000) {
        TEST_ASSERT_FALSE(net_retry_due(&r, t));
    }
}

TEST_CASE("connecting resets the backoff for the next outage", "[net]")
{
    net_retry_init(&r, 0);
    uint32_t now = 0;
    for (int i = 0; i < 8; i++) {
        net_retry_failed(&r, now);
        now += 60000;
    }
    TEST_ASSERT_EQUAL_UINT32(NET_RETRY_MAX_MS, net_retry_delay_ms(&r));

    net_retry_connected(&r);
    TEST_ASSERT_EQUAL_UINT32(NET_RETRY_FIRST_MS, net_retry_delay_ms(&r));
}

TEST_CASE("a dropped link retries at once, not after a minute", "[net]")
{
    net_retry_init(&r, 0);
    net_retry_connected(&r);

    /* A router reboot should cost a second, not the backoff left over
     * from whenever the last failure happened. */
    net_retry_disconnected(&r, 500000);
    TEST_ASSERT_TRUE(net_retry_due(&r, 500000));
    TEST_ASSERT_EQUAL_UINT32(NET_RETRY_FIRST_MS, net_retry_delay_ms(&r));
}

TEST_CASE("it never gives up", "[net]")
{
    net_retry_init(&r, 0);
    uint32_t now = 0;
    /* A week offline. */
    for (int i = 0; i < 7 * 24 * 60; i++) {
        net_retry_failed(&r, now);
        now += NET_RETRY_MAX_MS;
        TEST_ASSERT_TRUE(net_retry_due(&r, now));
    }
}

TEST_CASE("logging quietens but never stops", "[net]")
{
    net_retry_init(&r, 0);
    uint32_t now = 0;

    /* The first few failures each say something. */
    for (int i = 0; i < NET_RETRY_QUIET_AFTER; i++) {
        net_retry_failed(&r, now);
        now += 1000;
        TEST_ASSERT_TRUE(net_retry_should_log(&r));
    }
    /* Then, at the ceiling, one line per minute rather than silence:
     * an operator reading the log must be able to tell the difference
     * between "offline" and "crashed". */
    for (int i = 0; i < 50; i++) {
        net_retry_failed(&r, now);
        now += NET_RETRY_MAX_MS;
    }
    TEST_ASSERT_TRUE(net_retry_should_log(&r));
}

TEST_CASE("the millisecond counter wrapping does not stall a retry", "[net]")
{
    uint32_t near_wrap = 0xFFFFF000u;
    net_retry_init(&r, near_wrap);
    net_retry_failed(&r, near_wrap); /* next try 1000 ms later, past the wrap */

    TEST_ASSERT_FALSE(net_retry_due(&r, near_wrap + 500));
    TEST_ASSERT_TRUE(net_retry_due(&r, near_wrap + 1000)); /* wrapped */
}
