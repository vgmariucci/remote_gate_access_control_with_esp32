/* Tests for tg_poll: the cadence and failure policy of the Telegram
 * courier. No network and no clock here - every test supplies its own
 * now_ms, which is the point of keeping this layer pure. */
#include "tg_poll.h"
#include "unity.h"

static tg_poll_ctx_t ctx;

static void fresh(uint64_t stored_offset)
{
    tg_poll_init(&ctx, stored_offset, 1000);
}

/* ---- the offset sent to getUpdates ---- */

TEST_CASE("a device that has consumed nothing asks for whatever is queued", "[tg_poll]")
{
    fresh(0);
    TEST_ASSERT_EQUAL_UINT64(0, tg_poll_next_offset(&ctx));
}

TEST_CASE("a restored offset asks for the one after it", "[tg_poll]")
{
    fresh(41);
    TEST_ASSERT_EQUAL_UINT64(42, tg_poll_next_offset(&ctx));
}

TEST_CASE("consuming an update moves what the next request asks for", "[tg_poll]")
{
    fresh(0);
    TEST_ASSERT_TRUE(tg_poll_consume(&ctx, 500));
    TEST_ASSERT_EQUAL_UINT64(501, tg_poll_next_offset(&ctx));
}

TEST_CASE("a null context asks for nothing rather than crashing", "[tg_poll]")
{
    TEST_ASSERT_EQUAL_UINT64(0, tg_poll_next_offset(NULL));
    TEST_ASSERT_FALSE(tg_poll_due(NULL, 0, true));
    TEST_ASSERT_FALSE(tg_poll_consume(NULL, 1));
    TEST_ASSERT_FALSE(tg_poll_should_persist(NULL));
}

/* ---- consuming, including what was refused ---- */

TEST_CASE("an update already behind the offset does not move it", "[tg_poll]")
{
    fresh(100);
    TEST_ASSERT_FALSE(tg_poll_consume(&ctx, 100));
    TEST_ASSERT_FALSE(tg_poll_consume(&ctx, 99));
    TEST_ASSERT_EQUAL_UINT64(101, tg_poll_next_offset(&ctx));
}

TEST_CASE("a refused message is still consumed, so it cannot wedge the channel", "[tg_poll]")
{
    /* The caller consumes every update it received, whatever tg_parse
     * said about the contents. If a forged line were left unconsumed,
     * Telegram would hand back the same line on every request until it
     * expired, and nothing behind it would ever be seen. */
    fresh(0);
    TEST_ASSERT_TRUE(tg_poll_consume(&ctx, 7)); /* failed the MAC */
    TEST_ASSERT_TRUE(tg_poll_consume(&ctx, 8)); /* good */
    TEST_ASSERT_EQUAL_UINT64(9, tg_poll_next_offset(&ctx));
}

TEST_CASE("a batch is consumed in order and leaves the highest id", "[tg_poll]")
{
    fresh(0);
    tg_poll_consume(&ctx, 10);
    tg_poll_consume(&ctx, 11);
    tg_poll_consume(&ctx, 12);
    TEST_ASSERT_EQUAL_UINT64(13, tg_poll_next_offset(&ctx));
}

TEST_CASE("an out-of-order id cannot drag the offset backwards", "[tg_poll]")
{
    fresh(0);
    tg_poll_consume(&ctx, 20);
    TEST_ASSERT_FALSE(tg_poll_consume(&ctx, 5));
    TEST_ASSERT_EQUAL_UINT64(21, tg_poll_next_offset(&ctx));
}

/* ---- persistence ---- */

TEST_CASE("a fresh context has nothing to persist", "[tg_poll]")
{
    fresh(77);
    TEST_ASSERT_FALSE(tg_poll_should_persist(&ctx));
}

TEST_CASE("moving the offset asks for a write", "[tg_poll]")
{
    fresh(0);
    tg_poll_consume(&ctx, 3);
    TEST_ASSERT_TRUE(tg_poll_should_persist(&ctx));
}

TEST_CASE("a refused duplicate does not ask for a write", "[tg_poll]")
{
    fresh(50);
    tg_poll_consume(&ctx, 50);
    TEST_ASSERT_FALSE(tg_poll_should_persist(&ctx));
}

TEST_CASE("the flag clears once the write has happened", "[tg_poll]")
{
    fresh(0);
    tg_poll_consume(&ctx, 3);
    tg_poll_persisted(&ctx);
    TEST_ASSERT_FALSE(tg_poll_should_persist(&ctx));
    TEST_ASSERT_EQUAL_UINT64(4, tg_poll_next_offset(&ctx));
}

/* ---- when a request may run ---- */

TEST_CASE("the first request is due immediately", "[tg_poll]")
{
    fresh(0);
    TEST_ASSERT_TRUE(tg_poll_due(&ctx, 1000, true));
}

TEST_CASE("offline is never due", "[tg_poll]")
{
    fresh(0);
    TEST_ASSERT_FALSE(tg_poll_due(&ctx, 1000, false));
    TEST_ASSERT_FALSE(tg_poll_due(&ctx, 9999999, false));
}

TEST_CASE("an uninitialised context is never due", "[tg_poll]")
{
    tg_poll_ctx_t raw = {0};
    TEST_ASSERT_FALSE(tg_poll_due(&raw, 1000, true));
}

TEST_CASE("a completed request comes round again after the gap", "[tg_poll]")
{
    /* A long poll is a continuous conversation, not a timer: the server
     * already held the last request open for us. The gap is only there
     * to stop a server that answers instantly from turning the
     * conversation into a busy loop. */
    fresh(0);
    tg_poll_ok(&ctx, 60000);
    TEST_ASSERT_FALSE(tg_poll_due(&ctx, 60000, true));
    TEST_ASSERT_TRUE(tg_poll_due(&ctx, 60000 + TG_MIN_GAP_MS, true));
}

TEST_CASE("a server answering instantly cannot be spun against", "[tg_poll]")
{
    /* Telegram is meant to hold the request for TG_LONGPOLL_S. A
     * misconfigured or rate-limiting server that returns 200 with
     * nothing in it immediately is a success as far as the transport can
     * tell, so the floor is the only thing standing between that and a
     * tight loop of TLS handshakes. */
    fresh(0);
    uint32_t t = 1000; /* fresh() inits here */
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_TRUE(tg_poll_due(&ctx, t, true));
        tg_poll_ok(&ctx, t); /* answered in zero milliseconds */
        TEST_ASSERT_FALSE(tg_poll_due(&ctx, t, true));
        t += TG_MIN_GAP_MS;
    }
    /* 100 requests have taken at least 100 seconds, not zero. */
    TEST_ASSERT_EQUAL_UINT32(1000u + 100u * TG_MIN_GAP_MS, t);
}

TEST_CASE("the gap is small enough to keep the channel worth having", "[tg_poll]")
{
    /* The point of the push is seconds instead of fifteen minutes. A
     * floor anywhere near the sync period would defeat it. */
    TEST_ASSERT_TRUE(TG_MIN_GAP_MS <= 2000u);
}

/* ---- failing quietly ---- */

TEST_CASE("the first failure waits the minimum backoff", "[tg_poll]")
{
    fresh(0);
    tg_poll_failed(&ctx, 1000);
    TEST_ASSERT_FALSE(tg_poll_due(&ctx, 1000 + TG_BACKOFF_MIN_MS - 1, true));
    TEST_ASSERT_TRUE(tg_poll_due(&ctx, 1000 + TG_BACKOFF_MIN_MS, true));
}

TEST_CASE("each further failure doubles the wait", "[tg_poll]")
{
    fresh(0);
    tg_poll_failed(&ctx, 0);
    TEST_ASSERT_EQUAL_UINT32(TG_BACKOFF_MIN_MS, ctx.backoff_ms);
    tg_poll_failed(&ctx, 0);
    TEST_ASSERT_EQUAL_UINT32(TG_BACKOFF_MIN_MS * 2, ctx.backoff_ms);
    tg_poll_failed(&ctx, 0);
    TEST_ASSERT_EQUAL_UINT32(TG_BACKOFF_MIN_MS * 4, ctx.backoff_ms);
}

TEST_CASE("the backoff stops at its cap and never overflows", "[tg_poll]")
{
    fresh(0);
    for (int i = 0; i < 40; i++) {
        tg_poll_failed(&ctx, 0);
        TEST_ASSERT_TRUE(ctx.backoff_ms >= TG_BACKOFF_MIN_MS);
        TEST_ASSERT_TRUE(ctx.backoff_ms <= TG_BACKOFF_MAX_MS);
    }
    TEST_ASSERT_EQUAL_UINT32(TG_BACKOFF_MAX_MS, ctx.backoff_ms);
}

TEST_CASE("the cap stays under the sync period it is standing in for", "[tg_poll]")
{
    /* Once the retry interval exceeds the fifteen-minute poll, this
     * channel has stopped being an optimisation (ADR 0007). */
    TEST_ASSERT_TRUE(TG_BACKOFF_MAX_MS < 15u * 60u * 1000u);
}

TEST_CASE("a success clears the backoff entirely", "[tg_poll]")
{
    fresh(0);
    tg_poll_failed(&ctx, 0);
    tg_poll_failed(&ctx, 0);
    TEST_ASSERT_EQUAL_UINT32(2, ctx.consecutive_failures);

    tg_poll_ok(&ctx, 100000);
    TEST_ASSERT_EQUAL_UINT32(0, ctx.backoff_ms);
    TEST_ASSERT_EQUAL_UINT32(0, ctx.consecutive_failures);
    TEST_ASSERT_TRUE(tg_poll_due(&ctx, 100000 + TG_MIN_GAP_MS, true));
}

TEST_CASE("failing does not disturb the offset", "[tg_poll]")
{
    /* Nothing was consumed, so nothing may be acknowledged. */
    fresh(64);
    tg_poll_failed(&ctx, 0);
    tg_poll_failed(&ctx, 0);
    TEST_ASSERT_EQUAL_UINT64(65, tg_poll_next_offset(&ctx));
    TEST_ASSERT_FALSE(tg_poll_should_persist(&ctx));
}

/* ---- the 49-day wrap ---- */

TEST_CASE("a deadline that wraps past zero is still reached", "[tg_poll]")
{
    /* A plain `now < due` comparison would hold the channel shut for 49
     * days after the counter wraps. A gate runs for months. */
    const uint32_t near_wrap = 0xFFFFF000u;
    tg_poll_init(&ctx, 0, near_wrap);
    tg_poll_failed(&ctx, near_wrap); /* due at near_wrap + 5000, past the wrap */

    TEST_ASSERT_FALSE(tg_poll_due(&ctx, near_wrap + 1000, true));
    TEST_ASSERT_TRUE(tg_poll_due(&ctx, near_wrap + TG_BACKOFF_MIN_MS, true));
}

TEST_CASE("a request completed just before the wrap comes round after it", "[tg_poll]")
{
    const uint32_t near_wrap = 0xFFFFFF00u;
    tg_poll_init(&ctx, 0, near_wrap);
    tg_poll_ok(&ctx, near_wrap); /* next due past the wrap */
    TEST_ASSERT_FALSE(tg_poll_due(&ctx, near_wrap + 100u, true));
    TEST_ASSERT_TRUE(tg_poll_due(&ctx, near_wrap + TG_MIN_GAP_MS, true));
}

/* ---- failures that retrying cannot fix ---- */

TEST_CASE("a fatal failure stops the channel for good", "[tg_poll]")
{
    fresh(0);
    TEST_ASSERT_TRUE(tg_poll_due(&ctx, 1000, true));

    tg_poll_fatal(&ctx);
    TEST_ASSERT_TRUE(tg_poll_is_fatal(&ctx));
    TEST_ASSERT_FALSE(tg_poll_due(&ctx, 1000, true));
    TEST_ASSERT_FALSE(tg_poll_due(&ctx, 0xFFFFFFFFu, true));
}

TEST_CASE("a success cannot revive a stopped channel", "[tg_poll]")
{
    /* Nothing the transport observes should clear this: a wrong token
     * stays wrong until someone changes it. */
    fresh(0);
    tg_poll_fatal(&ctx);
    tg_poll_ok(&ctx, 2000);
    TEST_ASSERT_TRUE(tg_poll_is_fatal(&ctx));
    TEST_ASSERT_FALSE(tg_poll_due(&ctx, 999999, true));
}

TEST_CASE("only init clears the fatal state", "[tg_poll]")
{
    fresh(0);
    tg_poll_fatal(&ctx);
    tg_poll_init(&ctx, 12, 5000);
    TEST_ASSERT_FALSE(tg_poll_is_fatal(&ctx));
    TEST_ASSERT_TRUE(tg_poll_due(&ctx, 5000, true));
    TEST_ASSERT_EQUAL_UINT64(13, tg_poll_next_offset(&ctx));
}

TEST_CASE("a stopped channel keeps the offset it had reached", "[tg_poll]")
{
    /* The updates really were consumed; stopping is not a reason to
     * pretend otherwise and ask for them again later. */
    fresh(0);
    tg_poll_consume(&ctx, 90);
    tg_poll_fatal(&ctx);
    TEST_ASSERT_EQUAL_UINT64(91, tg_poll_next_offset(&ctx));
    TEST_ASSERT_TRUE(tg_poll_should_persist(&ctx));
}

TEST_CASE("fatal is null-safe", "[tg_poll]")
{
    tg_poll_fatal(NULL);
    TEST_ASSERT_FALSE(tg_poll_is_fatal(NULL));
}
