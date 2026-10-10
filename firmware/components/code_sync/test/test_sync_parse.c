/*
 * Unity tests for sync_parse.
 *
 * This body arrives over the network from a host the device cannot
 * otherwise verify. The cases that matter are the ones where it has
 * been tampered with, cut short, or is simply meant for someone else.
 */
#include <stdio.h>
#include <string.h>

#include "sync_parse.h"
#include "unity.h"

/* Stand-in for hmac-sha256: the tests need "matches" and "does not
 * match", not real crypto. */
static bool fake_mac(const char *region, size_t len, char out[SYNC_MAC_HEX_LEN + 1],
                     void *user)
{
    bool *should_fail = (bool *)user;
    if (should_fail != NULL && *should_fail) {
        memset(out, 'f', SYNC_MAC_HEX_LEN);
        out[SYNC_MAC_HEX_LEN] = '\0';
        return true;
    }
    unsigned h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h = (h ^ (unsigned char)region[i]) * 16777619u;
    }
    for (int i = 0; i < SYNC_MAC_HEX_LEN; i++) {
        out[i] = "0123456789abcdef"[(h >> ((i % 8) * 4)) & 0xF];
    }
    out[SYNC_MAC_HEX_LEN] = '\0';
    return true;
}

/* Appends a correct "mac" line to a body ending in "end\n". */
static void sign_body(char *dst, size_t dst_size, const char *signed_part)
{
    char mac[SYNC_MAC_HEX_LEN + 1];
    fake_mac(signed_part, strlen(signed_part), mac, NULL);
    snprintf(dst, dst_size, "%smac %s\n", signed_part, mac);
}

#define H1 "1111111111111111111111111111111111111111111111111111111111111111"
#define H2 "2222222222222222222222222222222222222222222222222222222222222222"

static const char TWO_CODES[] = "portao-sync v1\n"
                                "gate portao-01\n"
                                "gen 7\n"
                                "code a1b2c3d4 " H1 " 1700000000 1700086400\n"
                                "code 00ff11ee " H2 " 1700000000 1700172800\n"
                                "end\n";

static char body[SYNC_BODY_MAX];
static sync_set_t set;

TEST_CASE("a signed set parses", "[syncp]")
{
    sign_body(body, sizeof(body), TWO_CODES);
    TEST_ASSERT_EQUAL_INT(SYNC_PARSE_OK, sync_parse(body, "portao-01", fake_mac, NULL, &set));
    TEST_ASSERT_EQUAL_UINT64(7, set.generation);
    TEST_ASSERT_EQUAL_UINT(2, set.count);
    TEST_ASSERT_EQUAL_STRING("a1b2c3d4", set.entries[0].id);
    TEST_ASSERT_EQUAL_UINT8(0x11, set.entries[0].hash[0]);
    TEST_ASSERT_EQUAL_INT64(1700086400, set.entries[0].valid_until);
    TEST_ASSERT_EQUAL_STRING("00ff11ee", set.entries[1].id);
}

TEST_CASE("an empty set is legitimate", "[syncp]")
{
    /* Mass revocation: no code lines at all, and it must verify. */
    sign_body(body, sizeof(body), "portao-sync v1\ngate portao-01\ngen 9\nend\n");
    TEST_ASSERT_EQUAL_INT(SYNC_PARSE_OK, sync_parse(body, "portao-01", fake_mac, NULL, &set));
    TEST_ASSERT_EQUAL_UINT(0, set.count);
    TEST_ASSERT_EQUAL_UINT64(9, set.generation);
}

TEST_CASE("a forged signature is refused", "[syncp]")
{
    sign_body(body, sizeof(body), TWO_CODES);
    bool fail = true;
    TEST_ASSERT_EQUAL_INT(SYNC_PARSE_MAC,
                          sync_parse(body, "portao-01", fake_mac, &fail, &set));
}

TEST_CASE("moving an expiry invalidates the signature", "[syncp]")
{
    sign_body(body, sizeof(body), TWO_CODES);
    char *until = strstr(body, "1700086400");
    TEST_ASSERT_NOT_NULL(until);
    memcpy(until, "1700999999", 10);
    TEST_ASSERT_EQUAL_INT(SYNC_PARSE_MAC, sync_parse(body, "portao-01", fake_mac, NULL, &set));
}

TEST_CASE("a truncated body does not verify", "[syncp]")
{
    /* The attack the end marker exists for: cut the file after the
     * first code and the rest is silently revoked. Without signing a
     * terminator, the shorter body would carry its own valid
     * signature. */
    sign_body(body, sizeof(body), TWO_CODES);
    char *second = strstr(body, "code 00ff11ee");
    TEST_ASSERT_NOT_NULL(second);
    *second = '\0';
    TEST_ASSERT_EQUAL_INT(SYNC_PARSE_TRUNCATED,
                          sync_parse(body, "portao-01", fake_mac, NULL, &set));
}

TEST_CASE("a set for another gate is refused", "[syncp]")
{
    /* One mis-pointed URL must not load a neighbour's codes. */
    sign_body(body, sizeof(body), TWO_CODES);
    TEST_ASSERT_EQUAL_INT(SYNC_PARSE_GATE,
                          sync_parse(body, "portao-02", fake_mac, NULL, &set));
}

TEST_CASE("a future version is refused rather than guessed at", "[syncp]")
{
    sign_body(body, sizeof(body), "portao-sync v2\ngate portao-01\ngen 1\nend\n");
    TEST_ASSERT_EQUAL_INT(SYNC_PARSE_VERSION,
                          sync_parse(body, "portao-01", fake_mac, NULL, &set));
}

TEST_CASE("the network cannot deliver a dev id", "[syncp]")
{
    sign_body(body, sizeof(body),
              "portao-sync v1\ngate portao-01\ngen 1\n"
              "code dev00001 " H1 " 1 2\nend\n");
    TEST_ASSERT_EQUAL_INT(SYNC_PARSE_ENTRY,
                          sync_parse(body, "portao-01", fake_mac, NULL, &set));
}

TEST_CASE("a malformed code line refuses the whole set", "[syncp]")
{
    sign_body(body, sizeof(body),
              "portao-sync v1\ngate portao-01\ngen 1\n"
              "code a1b2c3d4 " H1 " 1700000000 1700086400\n"
              "code 00ff11ee short 1 2\nend\n");
    TEST_ASSERT_EQUAL_INT(SYNC_PARSE_ENTRY,
                          sync_parse(body, "portao-01", fake_mac, NULL, &set));
    /* Nothing half-applied: a caller that ignores the result gets an
     * empty set rather than the first code on its own. */
    TEST_ASSERT_EQUAL_UINT(0, set.count);
}

TEST_CASE("an inverted window is refused", "[syncp]")
{
    sign_body(body, sizeof(body),
              "portao-sync v1\ngate portao-01\ngen 1\n"
              "code a1b2c3d4 " H1 " 200 100\nend\n");
    TEST_ASSERT_EQUAL_INT(SYNC_PARSE_ENTRY,
                          sync_parse(body, "portao-01", fake_mac, NULL, &set));
}

TEST_CASE("more codes than we can hold is reported, not truncated", "[syncp]")
{
    char big[SYNC_BODY_MAX];
    int n = snprintf(big, sizeof(big), "portao-sync v1\ngate portao-01\ngen 1\n");
    for (int i = 0; i < SYNC_MAX_ENTRIES + 2; i++) {
        n += snprintf(big + n, sizeof(big) - n, "code a1b2c3%02x " H1 " 1 2\n", i);
    }
    snprintf(big + n, sizeof(big) - n, "end\n");
    sign_body(body, sizeof(body), big);
    TEST_ASSERT_EQUAL_INT(SYNC_PARSE_TOO_MANY,
                          sync_parse(body, "portao-01", fake_mac, NULL, &set));
}

TEST_CASE("junk from the network does not crash", "[syncp]")
{
    TEST_ASSERT_EQUAL_INT(SYNC_PARSE_FORMAT,
                          sync_parse("", "portao-01", fake_mac, NULL, &set));
    TEST_ASSERT_EQUAL_INT(SYNC_PARSE_TRUNCATED,
                          sync_parse("hello", "portao-01", fake_mac, NULL, &set));
    TEST_ASSERT_EQUAL_INT(SYNC_PARSE_FORMAT,
                          sync_parse(NULL, "portao-01", fake_mac, NULL, &set));
    TEST_ASSERT_EQUAL_INT(SYNC_PARSE_TRUNCATED,
                          sync_parse("\n\n\n\n", "portao-01", fake_mac, NULL, &set));

    /* A body longer than the buffer is refused on length. */
    static char huge[SYNC_BODY_MAX * 2];
    memset(huge, 'x', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(SYNC_PARSE_FORMAT,
                          sync_parse(huge, "portao-01", fake_mac, NULL, &set));
}

TEST_CASE("a missing mac line is refused", "[syncp]")
{
    TEST_ASSERT_EQUAL_INT(SYNC_PARSE_FORMAT,
                          sync_parse(TWO_CODES, "portao-01", fake_mac, NULL, &set));
}

TEST_CASE("a trailing carriage return does not break the signature", "[syncp]")
{
    /* A file edited on Windows, or served by something that rewrites
     * line endings on the last line only. */
    char mac[SYNC_MAC_HEX_LEN + 1];
    fake_mac(TWO_CODES, strlen(TWO_CODES), mac, NULL);
    snprintf(body, sizeof(body), "%smac %s\r\n", TWO_CODES, mac);
    TEST_ASSERT_EQUAL_INT(SYNC_PARSE_OK, sync_parse(body, "portao-01", fake_mac, NULL, &set));
}
