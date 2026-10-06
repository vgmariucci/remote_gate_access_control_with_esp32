/*
 * Unity tests for tg_proto.
 *
 * Every byte here arrived from a public chat. The cases that matter are
 * the ones where it is hostile: a forged signature, a replayed message,
 * an id that tries to look like a dev code.
 */
#include <stdio.h>
#include <string.h>

#include "tg_proto.h"
#include "unity.h"

/* A stand-in for hmac-sha256: the tests need "matches" and "does not
 * match", not real crypto. The real one is mbedtls, on the target. */
static bool fake_mac(const char *region, size_t len, char out[TG_MAC_HEX_LEN + 1], void *user)
{
    bool *should_fail = (bool *)user;
    if (should_fail != NULL && *should_fail) {
        memset(out, 'f', TG_MAC_HEX_LEN);
        out[TG_MAC_HEX_LEN] = '\0';
        return true;
    }
    /* Deterministic function of the signed region, so changing any byte
     * of the message changes the expected signature. */
    unsigned h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h = (h ^ (unsigned char)region[i]) * 16777619u;
    }
    for (int i = 0; i < TG_MAC_HEX_LEN; i++) {
        out[i] = "0123456789abcdef"[(h >> ((i % 8) * 4)) & 0xF];
    }
    out[TG_MAC_HEX_LEN] = '\0';
    return true;
}

/* Builds a correctly signed line from its parts. */
static void sign_line(char *dst, size_t dst_size, const char *body)
{
    char mac[TG_MAC_HEX_LEN + 1];
    fake_mac(body, strlen(body), mac, NULL);
    snprintf(dst, dst_size, "%s|%s", body, mac);
}

static const char *BODY_ADD =
    "v1|7|add|a1b2c3d4|"
    "0000000000000000000000000000000000000000000000000000000000000001|1700000000|1700086400";

static tg_msg_t msg;

TEST_CASE("a well-formed signed message is accepted", "[tg]")
{
    char line[TG_LINE_MAX];
    sign_line(line, sizeof(line), BODY_ADD);

    TEST_ASSERT_EQUAL_INT(TG_OK, tg_parse(line, 0, fake_mac, NULL, &msg));
    TEST_ASSERT_EQUAL_UINT64(7, msg.counter);
    TEST_ASSERT_EQUAL_INT(TG_OP_ADD, msg.op);
    TEST_ASSERT_EQUAL_STRING("a1b2c3d4", msg.id);
    TEST_ASSERT_EQUAL_UINT8(0x01, msg.hash[31]);
    TEST_ASSERT_EQUAL_INT64(1700000000, msg.valid_from);
    TEST_ASSERT_EQUAL_INT64(1700086400, msg.valid_until);
}

TEST_CASE("a forged signature is refused", "[tg]")
{
    char line[TG_LINE_MAX];
    sign_line(line, sizeof(line), BODY_ADD);
    bool fail = true;
    TEST_ASSERT_EQUAL_INT(TG_ERR_MAC, tg_parse(line, 0, fake_mac, &fail, &msg));
}

TEST_CASE("changing any byte invalidates the signature", "[tg]")
{
    char line[TG_LINE_MAX];
    sign_line(line, sizeof(line), BODY_ADD);

    /* Move the expiry a day later, as an attacker extending their own
     * stay would. The MAC covers it. */
    char *until = strstr(line, "1700086400");
    TEST_ASSERT_NOT_NULL(until);
    memcpy(until, "1700172800", 10);
    TEST_ASSERT_EQUAL_INT(TG_ERR_MAC, tg_parse(line, 0, fake_mac, NULL, &msg));
}

TEST_CASE("a replayed message is refused", "[tg]")
{
    char line[TG_LINE_MAX];
    sign_line(line, sizeof(line), BODY_ADD); /* counter 7 */

    TEST_ASSERT_EQUAL_INT(TG_OK, tg_parse(line, 6, fake_mac, NULL, &msg));
    TEST_ASSERT_EQUAL_INT(TG_ERR_REPLAY, tg_parse(line, 7, fake_mac, NULL, &msg));
    TEST_ASSERT_EQUAL_INT(TG_ERR_REPLAY, tg_parse(line, 8, fake_mac, NULL, &msg));
}

TEST_CASE("a revoke cannot be undone by replaying the original add", "[tg]")
{
    /* The scenario the counter exists for: an add at 7, a revoke at 8,
     * and the attacker re-posting the add they captured. */
    char add[TG_LINE_MAX];
    char rev[TG_LINE_MAX];
    sign_line(add, sizeof(add), BODY_ADD);
    sign_line(rev, sizeof(rev), "v1|8|rev|a1b2c3d4|0|0|0");

    TEST_ASSERT_EQUAL_INT(TG_OK, tg_parse(add, 0, fake_mac, NULL, &msg));
    TEST_ASSERT_EQUAL_INT(TG_OK, tg_parse(rev, 7, fake_mac, NULL, &msg));
    TEST_ASSERT_EQUAL_INT(TG_OP_REVOKE, msg.op);
    TEST_ASSERT_EQUAL_INT(TG_ERR_REPLAY, tg_parse(add, 8, fake_mac, NULL, &msg));
}

TEST_CASE("the network cannot mint a dev id", "[tg]")
{
    /* ac_id_is_dev() calls anything with a non-hex character a dev code.
     * If a delivered message could carry one, the chat could create or
     * collide with a console session's slot. */
    char line[TG_LINE_MAX];
    sign_line(line, sizeof(line),
              "v1|7|add|dev00001|"
              "0000000000000000000000000000000000000000000000000000000000000001|1|2");
    TEST_ASSERT_EQUAL_INT(TG_ERR_ID, tg_parse(line, 0, fake_mac, NULL, &msg));

    sign_line(line, sizeof(line),
              "v1|7|add|tab9ae0b|"
              "0000000000000000000000000000000000000000000000000000000000000001|1|2");
    TEST_ASSERT_EQUAL_INT(TG_ERR_ID, tg_parse(line, 0, fake_mac, NULL, &msg));
}

TEST_CASE("an id of the wrong length is refused", "[tg]")
{
    char line[TG_LINE_MAX];
    sign_line(line, sizeof(line),
              "v1|7|add|a1b2c3|"
              "0000000000000000000000000000000000000000000000000000000000000001|1|2");
    TEST_ASSERT_EQUAL_INT(TG_ERR_ID, tg_parse(line, 0, fake_mac, NULL, &msg));

    sign_line(line, sizeof(line),
              "v1|7|add|a1b2c3d4e5|"
              "0000000000000000000000000000000000000000000000000000000000000001|1|2");
    TEST_ASSERT_EQUAL_INT(TG_ERR_ID, tg_parse(line, 0, fake_mac, NULL, &msg));
}

TEST_CASE("uppercase hex is refused, so one message has one signature", "[tg]")
{
    char line[TG_LINE_MAX];
    sign_line(line, sizeof(line),
              "v1|7|add|A1B2C3D4|"
              "0000000000000000000000000000000000000000000000000000000000000001|1|2");
    TEST_ASSERT_EQUAL_INT(TG_ERR_ID, tg_parse(line, 0, fake_mac, NULL, &msg));
}

TEST_CASE("a short or non-hex hash is refused", "[tg]")
{
    char line[TG_LINE_MAX];
    sign_line(line, sizeof(line), "v1|7|add|a1b2c3d4|00112233|1|2");
    TEST_ASSERT_EQUAL_INT(TG_ERR_HASH, tg_parse(line, 0, fake_mac, NULL, &msg));

    sign_line(line, sizeof(line),
              "v1|7|add|a1b2c3d4|"
              "zz00000000000000000000000000000000000000000000000000000000000001|1|2");
    TEST_ASSERT_EQUAL_INT(TG_ERR_HASH, tg_parse(line, 0, fake_mac, NULL, &msg));
}

TEST_CASE("an inverted validity window is refused", "[tg]")
{
    char line[TG_LINE_MAX];
    sign_line(line, sizeof(line),
              "v1|7|add|a1b2c3d4|"
              "0000000000000000000000000000000000000000000000000000000000000001|200|100");
    TEST_ASSERT_EQUAL_INT(TG_ERR_WINDOW, tg_parse(line, 0, fake_mac, NULL, &msg));
}

TEST_CASE("an unknown operation is refused", "[tg]")
{
    char line[TG_LINE_MAX];
    sign_line(line, sizeof(line),
              "v1|7|del|a1b2c3d4|"
              "0000000000000000000000000000000000000000000000000000000000000001|1|2");
    TEST_ASSERT_EQUAL_INT(TG_ERR_OP, tg_parse(line, 0, fake_mac, NULL, &msg));
}

TEST_CASE("a future version is refused rather than guessed at", "[tg]")
{
    char line[TG_LINE_MAX];
    sign_line(line, sizeof(line),
              "v2|7|add|a1b2c3d4|"
              "0000000000000000000000000000000000000000000000000000000000000001|1|2");
    TEST_ASSERT_EQUAL_INT(TG_ERR_VERSION, tg_parse(line, 0, fake_mac, NULL, &msg));
}

TEST_CASE("the wrong number of fields is refused", "[tg]")
{
    char line[TG_LINE_MAX];
    sign_line(line, sizeof(line), "v1|7|add|a1b2c3d4");
    TEST_ASSERT_EQUAL_INT(TG_ERR_FORMAT, tg_parse(line, 0, fake_mac, NULL, &msg));

    sign_line(line, sizeof(line),
              "v1|7|add|a1b2c3d4|"
              "0000000000000000000000000000000000000000000000000000000000000001|1|2|extra");
    TEST_ASSERT_EQUAL_INT(TG_ERR_FORMAT, tg_parse(line, 0, fake_mac, NULL, &msg));
}

TEST_CASE("junk from the chat does not crash", "[tg]")
{
    TEST_ASSERT_EQUAL_INT(TG_ERR_FORMAT, tg_parse("", 0, fake_mac, NULL, &msg));
    /* Eight empty fields is the right shape and the wrong version,
     * which is what it is reported as. */
    TEST_ASSERT_EQUAL_INT(TG_ERR_VERSION, tg_parse("|||||||", 0, fake_mac, NULL, &msg));
    TEST_ASSERT_EQUAL_INT(TG_ERR_FORMAT, tg_parse(NULL, 0, fake_mac, NULL, &msg));
    TEST_ASSERT_EQUAL_INT(TG_ERR_FORMAT, tg_parse("hello everyone", 0, fake_mac, NULL, &msg));

    /* Longer than the buffer: refused on length, never copied. */
    char huge[TG_LINE_MAX * 2];
    memset(huge, 'a', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(TG_ERR_FORMAT, tg_parse(huge, 0, fake_mac, NULL, &msg));
}

TEST_CASE("a counter that is not a number is refused", "[tg]")
{
    char line[TG_LINE_MAX];
    sign_line(line, sizeof(line),
              "v1|-1|add|a1b2c3d4|"
              "0000000000000000000000000000000000000000000000000000000000000001|1|2");
    TEST_ASSERT_EQUAL_INT(TG_ERR_FORMAT, tg_parse(line, 0, fake_mac, NULL, &msg));
}

TEST_CASE("nothing is written to the output when parsing fails", "[tg]")
{
    char line[TG_LINE_MAX];
    sign_line(line, sizeof(line), BODY_ADD);
    TEST_ASSERT_EQUAL_INT(TG_OK, tg_parse(line, 0, fake_mac, NULL, &msg));
    TEST_ASSERT_EQUAL_STRING("a1b2c3d4", msg.id);

    /* A failure must not leave the previous message's id behind, or a
     * caller that ignores the return value acts on stale data. */
    bool fail = true;
    TEST_ASSERT_EQUAL_INT(TG_ERR_MAC, tg_parse(line, 0, fake_mac, &fail, &msg));
    TEST_ASSERT_EQUAL_STRING("", msg.id);
    TEST_ASSERT_EQUAL_UINT64(0, msg.counter);
}
