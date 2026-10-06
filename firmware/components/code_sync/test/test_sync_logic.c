#include <stdio.h>
/*
 * Unity tests for sync_logic and status_led.
 *
 * ADR 0007: the poll is the correctness mechanism, so these cases are
 * about the ways a gate ends up quietly holding the wrong codes.
 */
#include <string.h>

#include "status_led.h"
#include "sync_logic.h"
#include "unity.h"

static sync_ctx_t ctx;
static ac_ctx_t table;
static sync_set_t set;

static void fill(sync_set_t *s, uint64_t gen, size_t count)
{
    memset(s, 0, sizeof(*s));
    s->generation = gen;
    s->count = count;
    for (size_t i = 0; i < count; i++) {
        snprintf(s->entries[i].id, AC_ID_LEN, "a1b2c3%02x", (unsigned)i);
        memset(s->entries[i].hash, (int)(i + 1), AC_HASH_LEN);
        s->entries[i].valid_from = 1000;
        s->entries[i].valid_until = 2000;
    }
}

static void fixture(void)
{
    ac_init(&table, 5, 300);
    table.clock_trusted = true;
    sync_init(&ctx, 0, 0);
}

TEST_CASE("a first set installs", "[sync]")
{
    fixture();
    fill(&set, 1, 3);
    TEST_ASSERT_EQUAL_INT(SYNC_APPLIED, sync_apply(&ctx, &table, &set, 0));
    TEST_ASSERT_EQUAL_UINT(3, ac_count(&table));
    TEST_ASSERT_EQUAL_UINT64(1, ctx.generation);
}

TEST_CASE("a newer set replaces rather than merges", "[sync]")
{
    fixture();
    fill(&set, 1, 3);
    sync_apply(&ctx, &table, &set, 0);

    /* Generation 2 holds one code. The other two were revoked, and
     * their absence is how the device is told. */
    fill(&set, 2, 1);
    TEST_ASSERT_EQUAL_INT(SYNC_APPLIED, sync_apply(&ctx, &table, &set, 1000));
    TEST_ASSERT_EQUAL_UINT(1, ac_count(&table));
}

TEST_CASE("an empty set empties the table", "[sync]")
{
    /* Mass revocation has to work; a backend bug that sends this by
     * mistake is the accepted cost (ADR 0007). */
    fixture();
    fill(&set, 1, 3);
    sync_apply(&ctx, &table, &set, 0);

    fill(&set, 2, 0);
    TEST_ASSERT_EQUAL_INT(SYNC_APPLIED, sync_apply(&ctx, &table, &set, 1000));
    TEST_ASSERT_EQUAL_UINT(0, ac_count(&table));
}

TEST_CASE("a replayed set cannot restore a revoked code", "[sync]")
{
    fixture();
    sync_set_t old_set;
    fill(&old_set, 5, 3);
    sync_apply(&ctx, &table, &old_set, 0);

    fill(&set, 6, 0); /* everything revoked */
    sync_apply(&ctx, &table, &set, 1000);
    TEST_ASSERT_EQUAL_UINT(0, ac_count(&table));

    /* The attacker replays the set they captured. It is correctly
     * signed and it is refused anyway. */
    TEST_ASSERT_EQUAL_INT(SYNC_STALE, sync_apply(&ctx, &table, &old_set, 2000));
    TEST_ASSERT_EQUAL_UINT(0, ac_count(&table));
}

TEST_CASE("the same generation is a no-op", "[sync]")
{
    fixture();
    fill(&set, 4, 2);
    TEST_ASSERT_EQUAL_INT(SYNC_APPLIED, sync_apply(&ctx, &table, &set, 0));
    TEST_ASSERT_EQUAL_INT(SYNC_UNCHANGED, sync_apply(&ctx, &table, &set, 1000));
    TEST_ASSERT_EQUAL_UINT(2, ac_count(&table));
}

TEST_CASE("a dev code survives a sync", "[sync]")
{
    /* The backend owns guest codes; a console session owns its own. */
    fixture();
    uint8_t h[AC_HASH_LEN];
    memset(h, 0xAB, sizeof(h));
    ac_upsert_transient(&table, "dev00001", h, 1000, 1200);

    fill(&set, 1, 2);
    TEST_ASSERT_EQUAL_INT(SYNC_APPLIED, sync_apply(&ctx, &table, &set, 0));
    TEST_ASSERT_EQUAL_UINT(3, ac_count(&table));

    bool found = false;
    for (int i = 0; i < AC_MAX_SLOTS; i++) {
        if (table.slots[i].occupied && strcmp(table.slots[i].id, "dev00001") == 0) {
            found = true;
            TEST_ASSERT_TRUE(table.slots[i].transient);
        }
    }
    TEST_ASSERT_TRUE(found);
}

TEST_CASE("a set the backend should not have sent is refused whole", "[sync]")
{
    fixture();
    fill(&set, 1, 2);
    sync_apply(&ctx, &table, &set, 0);

    /* A dev-looking id from the network, which would collide with a
     * console slot. Nothing is applied, not even the good entry. */
    fill(&set, 2, 2);
    snprintf(set.entries[1].id, AC_ID_LEN, "dev00009");
    TEST_ASSERT_EQUAL_INT(SYNC_BAD_ENTRY, sync_apply(&ctx, &table, &set, 1000));
    TEST_ASSERT_EQUAL_UINT(2, ac_count(&table)); /* the old set still stands */
    TEST_ASSERT_EQUAL_UINT64(1, ctx.generation);
}

TEST_CASE("an inverted window is refused whole", "[sync]")
{
    fixture();
    fill(&set, 1, 2);
    set.entries[0].valid_until = 500; /* before valid_from */
    TEST_ASSERT_EQUAL_INT(SYNC_BAD_ENTRY, sync_apply(&ctx, &table, &set, 0));
    TEST_ASSERT_EQUAL_UINT(0, ac_count(&table));
}

TEST_CASE("more codes than slots fills what fits and says so", "[sync]")
{
    fixture();
    fill(&set, 1, 7); /* the table holds five */
    TEST_ASSERT_EQUAL_INT(SYNC_OVERFLOWED, sync_apply(&ctx, &table, &set, 0));
    TEST_ASSERT_EQUAL_UINT(AC_MAX_SLOTS, ac_count(&table));
    /* Applied anyway: a partial table beats a stale one. */
    TEST_ASSERT_EQUAL_UINT64(1, ctx.generation);
}

TEST_CASE("the first poll is due immediately", "[sync]")
{
    fixture();
    TEST_ASSERT_TRUE(sync_due(&ctx, 0));
}

TEST_CASE("a success schedules the next poll a period away", "[sync]")
{
    fixture();
    fill(&set, 1, 1);
    sync_apply(&ctx, &table, &set, 1000);
    TEST_ASSERT_FALSE(sync_due(&ctx, 1000 + SYNC_PERIOD_MS - 1));
    TEST_ASSERT_TRUE(sync_due(&ctx, 1000 + SYNC_PERIOD_MS));
}

TEST_CASE("a failed poll does not retry in a tight loop", "[sync]")
{
    fixture();
    sync_failed(&ctx, 5000);
    TEST_ASSERT_FALSE(sync_due(&ctx, 5000 + 1000));
    TEST_ASSERT_TRUE(sync_due(&ctx, 5000 + SYNC_PERIOD_MS));
}

TEST_CASE("a gate that has never synced is stale", "[sync]")
{
    fixture();
    TEST_ASSERT_TRUE(sync_is_stale(&ctx, 0));
    fill(&set, 1, 1);
    sync_apply(&ctx, &table, &set, 1000);
    TEST_ASSERT_FALSE(sync_is_stale(&ctx, 1000));
}

TEST_CASE("staleness arrives an hour after the last success", "[sync]")
{
    fixture();
    fill(&set, 1, 1);
    sync_apply(&ctx, &table, &set, 1000);
    TEST_ASSERT_FALSE(sync_is_stale(&ctx, 1000 + SYNC_STALE_MS));
    TEST_ASSERT_TRUE(sync_is_stale(&ctx, 1000 + SYNC_STALE_MS + 1));
}

TEST_CASE("unchanged counts as a successful sync", "[sync]")
{
    /* The backend said nothing changed; the gate is up to date, so it
     * must not drift into amber for not having received a set. */
    fixture();
    fill(&set, 1, 1);
    sync_apply(&ctx, &table, &set, 0);
    sync_unchanged(&ctx, SYNC_STALE_MS);
    TEST_ASSERT_FALSE(sync_is_stale(&ctx, SYNC_STALE_MS + 1000));
}

/* ------------------------------------------------------------- LED */

TEST_CASE("the LED says what is most worth knowing", "[led]")
{
    led_inputs_t in = {.online = true, .clock_trusted = true};
    TEST_ASSERT_EQUAL_INT(LED_GREEN, led_colour(&in));

    in.online = false;
    TEST_ASSERT_EQUAL_INT(LED_RED, led_colour(&in));

    in.online = true;
    in.sync_stale = true;
    TEST_ASSERT_EQUAL_INT(LED_AMBER, led_colour(&in));

    in.portal_open = true;
    TEST_ASSERT_EQUAL_INT(LED_BLUE, led_colour(&in));

    in.clock_trusted = false;
    TEST_ASSERT_EQUAL_INT(LED_MAGENTA, led_colour(&in));
}

TEST_CASE("a gate refusing every code outranks everything else", "[led]")
{
    /* Whatever else is true, this is the one that explains why the
     * guest at the door cannot get in. */
    led_inputs_t in = {
        .online = true, .sync_stale = false, .portal_open = true, .clock_trusted = false};
    TEST_ASSERT_EQUAL_INT(LED_MAGENTA, led_colour(&in));
}

TEST_CASE("offline does not mask an open portal", "[led]")
{
    /* The AP is up and reachable even with no house network, and
     * someone should notice it. */
    led_inputs_t in = {.online = false, .portal_open = true, .clock_trusted = true};
    TEST_ASSERT_EQUAL_INT(LED_BLUE, led_colour(&in));
}

TEST_CASE("the blink is brief and periodic", "[led]")
{
    TEST_ASSERT_TRUE(led_is_on(0));
    TEST_ASSERT_TRUE(led_is_on(LED_BLINK_ON_MS - 1));
    TEST_ASSERT_FALSE(led_is_on(LED_BLINK_ON_MS));
    TEST_ASSERT_FALSE(led_is_on(LED_BLINK_PERIOD_MS / 2));
    TEST_ASSERT_TRUE(led_is_on(LED_BLINK_PERIOD_MS));

    /* Dark far more than it is lit: no beacon at night. */
    int lit = 0;
    for (uint32_t t = 0; t < LED_BLINK_PERIOD_MS; t += 10) {
        if (led_is_on(t)) {
            lit++;
        }
    }
    TEST_ASSERT_TRUE(lit * 20 < (int)(LED_BLINK_PERIOD_MS / 10));
}
