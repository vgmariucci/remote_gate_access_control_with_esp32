/*
 * Unit tests for access_core.
 *
 * These run on the host (linux target) in CI and on the device with
 * `idf.py -T access_core ...`. Same file, same assertions.
 */
#include <stdio.h>
#include <string.h>

#include "access_core.h"
#include "unity.h"

/* Deterministic fake hashes. The real ones come from mbedtls sha256;
 * access_core only ever compares bytes, so the test does not need it. */
static void fake_hash(uint8_t out[AC_HASH_LEN], uint8_t seed)
{
    for (int i = 0; i < AC_HASH_LEN; i++) {
        out[i] = (uint8_t)(seed + i);
    }
}

#define T0 1773360000LL /* an arbitrary fixed "now" */
#define HOUR 3600LL
#define DAY 86400LL

/* --------------------------------------------------------------- */
/* Format policy                                                     */
/* --------------------------------------------------------------- */

TEST_CASE("format: canonical valid codes are accepted", "[access_core]")
{
    TEST_ASSERT_TRUE(ac_format_valid("123456AB*"));   /* minimum length, 9 */
    TEST_ASSERT_TRUE(ac_format_valid("1A2B3*456"));   /* interleaved */
    TEST_ASSERT_TRUE(ac_format_valid("#DDCC009182")); /* extra letters and specials */
}

TEST_CASE("format: wrong digit count is rejected", "[access_core]")
{
    TEST_ASSERT_FALSE(ac_format_valid("12345AB*"));   /* 5 digits */
    TEST_ASSERT_FALSE(ac_format_valid("1234567AB*")); /* 7 digits */
}

TEST_CASE("format: letter and special minimums are enforced", "[access_core]")
{
    TEST_ASSERT_FALSE(ac_format_valid("123456A*")); /* only 1 letter */
    TEST_ASSERT_FALSE(ac_format_valid("123456AB")); /* no special */
    TEST_ASSERT_FALSE(ac_format_valid("123456"));   /* digits only */
}

TEST_CASE("format: characters absent from a 4x4 keypad are rejected", "[access_core]")
{
    TEST_ASSERT_FALSE(ac_format_valid("123456EF*")); /* E and F are not keys */
    TEST_ASSERT_FALSE(ac_format_valid("123456ab*")); /* lowercase */
    TEST_ASSERT_FALSE(ac_format_valid("123456AB!"));
}

TEST_CASE("format: empty, NULL and overlong inputs are rejected", "[access_core]")
{
    TEST_ASSERT_FALSE(ac_format_valid(NULL));
    TEST_ASSERT_FALSE(ac_format_valid(""));
    TEST_ASSERT_FALSE(ac_format_valid("123456ABCD*#ABCD*#"));
}

/* --------------------------------------------------------------- */
/* Slot table                                                        */
/* --------------------------------------------------------------- */

TEST_CASE("upsert: replaces by id rather than consuming a second slot", "[access_core]")
{
    ac_ctx_t ctx;
    uint8_t h1[AC_HASH_LEN], h2[AC_HASH_LEN];
    fake_hash(h1, 1);
    fake_hash(h2, 2);

    ac_init(&ctx, 5, 60);
    TEST_ASSERT_GREATER_OR_EQUAL(0, ac_upsert(&ctx, "aaaa1111", h1, T0, T0 + DAY));
    TEST_ASSERT_EQUAL_UINT(1, ac_count(&ctx));

    /* Same id, new hash: this is the re-push after sync_failed. */
    TEST_ASSERT_GREATER_OR_EQUAL(0, ac_upsert(&ctx, "aaaa1111", h2, T0, T0 + DAY));
    TEST_ASSERT_EQUAL_UINT(1, ac_count(&ctx));

    ctx.clock_trusted = true;
    TEST_ASSERT_EQUAL(AC_DENIED_UNKNOWN, ac_evaluate(&ctx, h1, T0 + HOUR, NULL));
    TEST_ASSERT_EQUAL(AC_GRANTED, ac_evaluate(&ctx, h2, T0 + HOUR, NULL));
}

TEST_CASE("upsert: rejects an inverted validity window", "[access_core]")
{
    ac_ctx_t ctx;
    uint8_t h[AC_HASH_LEN];
    fake_hash(h, 3);
    ac_init(&ctx, 5, 60);
    TEST_ASSERT_EQUAL_INT(-1, ac_upsert(&ctx, "bbbb2222", h, T0 + DAY, T0));
    TEST_ASSERT_EQUAL_UINT(0, ac_count(&ctx));
}

TEST_CASE("upsert: refuses to overflow the table", "[access_core]")
{
    ac_ctx_t ctx;
    uint8_t h[AC_HASH_LEN];
    char id[AC_ID_LEN];
    ac_init(&ctx, 5, 60);

    for (int i = 0; i < AC_MAX_SLOTS; i++) {
        fake_hash(h, (uint8_t)(10 + i));
        snprintf(id, sizeof(id), "id%06d", i);
        TEST_ASSERT_GREATER_OR_EQUAL(0, ac_upsert(&ctx, id, h, T0, T0 + DAY));
    }
    TEST_ASSERT_EQUAL_UINT(AC_MAX_SLOTS, ac_count(&ctx));

    fake_hash(h, 99);
    TEST_ASSERT_EQUAL_INT(-1, ac_upsert(&ctx, "overflow", h, T0, T0 + DAY));
}

TEST_CASE("revoke: removes the code immediately", "[access_core]")
{
    ac_ctx_t ctx;
    uint8_t h[AC_HASH_LEN];
    fake_hash(h, 4);

    ac_init(&ctx, 5, 60);
    ctx.clock_trusted = true;
    ac_upsert(&ctx, "cccc3333", h, T0, T0 + DAY);
    TEST_ASSERT_EQUAL(AC_GRANTED, ac_evaluate(&ctx, h, T0 + HOUR, NULL));

    TEST_ASSERT_TRUE(ac_revoke(&ctx, "cccc3333"));
    TEST_ASSERT_FALSE(ac_revoke(&ctx, "cccc3333")); /* idempotent */
    TEST_ASSERT_EQUAL(AC_DENIED_UNKNOWN, ac_evaluate(&ctx, h, T0 + HOUR, NULL));
}

TEST_CASE("purge: frees only windows that already closed", "[access_core]")
{
    ac_ctx_t ctx;
    uint8_t h1[AC_HASH_LEN], h2[AC_HASH_LEN];
    fake_hash(h1, 5);
    fake_hash(h2, 6);

    ac_init(&ctx, 5, 60);
    ac_upsert(&ctx, "old00001", h1, T0 - 2 * DAY, T0 - DAY);
    ac_upsert(&ctx, "live0001", h2, T0, T0 + DAY);

    TEST_ASSERT_EQUAL_INT(1, ac_purge_expired(&ctx, T0));
    TEST_ASSERT_EQUAL_UINT(1, ac_count(&ctx));
}

/* --------------------------------------------------------------- */
/* The decision                                                      */
/* --------------------------------------------------------------- */

TEST_CASE("evaluate: fails closed while the clock is untrusted", "[access_core]")
{
    ac_ctx_t ctx;
    uint8_t h[AC_HASH_LEN];
    fake_hash(h, 7);

    ac_init(&ctx, 5, 60);
    ac_upsert(&ctx, "dddd4444", h, T0, T0 + DAY);

    /* clock_trusted defaults to false: a valid code must still be refused */
    TEST_ASSERT_EQUAL(AC_DENIED_NO_CLOCK, ac_evaluate(&ctx, h, T0 + HOUR, NULL));

    ctx.clock_trusted = true;
    TEST_ASSERT_EQUAL(AC_GRANTED, ac_evaluate(&ctx, h, T0 + HOUR, NULL));
}

TEST_CASE("evaluate: distinguishes not-yet-valid from expired", "[access_core]")
{
    ac_ctx_t ctx;
    uint8_t h[AC_HASH_LEN];
    fake_hash(h, 8);

    ac_init(&ctx, 5, 60);
    ctx.clock_trusted = true;
    ac_upsert(&ctx, "eeee5555", h, T0, T0 + DAY);

    TEST_ASSERT_EQUAL(AC_DENIED_NOT_YET, ac_evaluate(&ctx, h, T0 - HOUR, NULL));
    TEST_ASSERT_EQUAL(AC_DENIED_EXPIRED, ac_evaluate(&ctx, h, T0 + DAY + HOUR, NULL));
}

TEST_CASE("evaluate: boundaries are inclusive at both ends", "[access_core]")
{
    ac_ctx_t ctx;
    uint8_t h[AC_HASH_LEN];
    fake_hash(h, 9);

    ac_init(&ctx, 5, 60);
    ctx.clock_trusted = true;
    ac_upsert(&ctx, "ffff6666", h, T0, T0 + DAY);

    TEST_ASSERT_EQUAL(AC_GRANTED, ac_evaluate(&ctx, h, T0, NULL));
    TEST_ASSERT_EQUAL(AC_GRANTED, ac_evaluate(&ctx, h, T0 + DAY, NULL));
}

TEST_CASE("evaluate: returns the matching id for attribution", "[access_core]")
{
    ac_ctx_t ctx;
    uint8_t h[AC_HASH_LEN];
    char id[AC_ID_LEN] = {0};
    fake_hash(h, 11);

    ac_init(&ctx, 5, 60);
    ctx.clock_trusted = true;
    ac_upsert(&ctx, "abcd1234", h, T0, T0 + DAY);

    TEST_ASSERT_EQUAL(AC_GRANTED, ac_evaluate(&ctx, h, T0 + HOUR, id));
    TEST_ASSERT_EQUAL_STRING("abcd1234", id);
}

/* --------------------------------------------------------------- */
/* Lockout                                                           */
/* --------------------------------------------------------------- */

TEST_CASE("lockout: arms after the configured number of failures", "[access_core]")
{
    ac_ctx_t ctx;
    uint8_t good[AC_HASH_LEN], bad[AC_HASH_LEN];
    fake_hash(good, 20);
    fake_hash(bad, 21);

    ac_init(&ctx, 3, 60);
    ctx.clock_trusted = true;
    ac_upsert(&ctx, "lock0001", good, T0, T0 + DAY);

    TEST_ASSERT_EQUAL(AC_DENIED_UNKNOWN, ac_evaluate(&ctx, bad, T0, NULL));
    TEST_ASSERT_EQUAL(AC_DENIED_UNKNOWN, ac_evaluate(&ctx, bad, T0, NULL));
    TEST_ASSERT_EQUAL(AC_DENIED_UNKNOWN, ac_evaluate(&ctx, bad, T0, NULL));

    /* Third failure armed the lockout: even the correct code is refused. */
    TEST_ASSERT_EQUAL(AC_DENIED_LOCKOUT, ac_evaluate(&ctx, good, T0 + 1, NULL));

    /* And it lifts on its own. */
    TEST_ASSERT_EQUAL(AC_GRANTED, ac_evaluate(&ctx, good, T0 + 61, NULL));
}

TEST_CASE("lockout: a success clears the failure counter", "[access_core]")
{
    ac_ctx_t ctx;
    uint8_t good[AC_HASH_LEN], bad[AC_HASH_LEN];
    fake_hash(good, 30);
    fake_hash(bad, 31);

    ac_init(&ctx, 3, 60);
    ctx.clock_trusted = true;
    ac_upsert(&ctx, "lock0002", good, T0, T0 + DAY);

    TEST_ASSERT_EQUAL(AC_DENIED_UNKNOWN, ac_evaluate(&ctx, bad, T0, NULL));
    TEST_ASSERT_EQUAL(AC_DENIED_UNKNOWN, ac_evaluate(&ctx, bad, T0, NULL));
    TEST_ASSERT_EQUAL(AC_GRANTED, ac_evaluate(&ctx, good, T0, NULL));

    /* Counter reset, so two more failures must not be enough. */
    TEST_ASSERT_EQUAL(AC_DENIED_UNKNOWN, ac_evaluate(&ctx, bad, T0, NULL));
    TEST_ASSERT_EQUAL(AC_DENIED_UNKNOWN, ac_evaluate(&ctx, bad, T0, NULL));
    TEST_ASSERT_EQUAL(AC_GRANTED, ac_evaluate(&ctx, good, T0, NULL));
}

TEST_CASE("lockout: an expired code still counts as a failure", "[access_core]")
{
    ac_ctx_t ctx;
    uint8_t h[AC_HASH_LEN];
    fake_hash(h, 40);

    ac_init(&ctx, 2, 60);
    ctx.clock_trusted = true;
    ac_upsert(&ctx, "lock0003", h, T0 - 2 * DAY, T0 - DAY);

    TEST_ASSERT_EQUAL(AC_DENIED_EXPIRED, ac_evaluate(&ctx, h, T0, NULL));
    TEST_ASSERT_EQUAL(AC_DENIED_EXPIRED, ac_evaluate(&ctx, h, T0, NULL));
    TEST_ASSERT_EQUAL(AC_DENIED_LOCKOUT, ac_evaluate(&ctx, h, T0, NULL));
}
