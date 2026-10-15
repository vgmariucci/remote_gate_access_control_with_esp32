/* Rules for the Telegram bot token.
 *
 * In its own file rather than appended to test_cred_check.c: the token
 * is the first credential whose failure must not stop the gate, and
 * keeping that distinction visible is worth a file. */
#include "cred_check.h"
#include "unity.h"

/* Shaped like a real one: 9 digits, a colon, 35 characters. */
#define GOOD_TOKEN "123456789:AAHdqTcvCH1vGWJxfSeofSAs0K5PALDsabc"

TEST_CASE("a well-formed token is accepted", "[cred_tg]")
{
    TEST_ASSERT_EQUAL(CRED_OK, cred_check_bot_token(GOOD_TOKEN));
}

TEST_CASE("the example file's own token is refused", "[cred_tg]")
{
    /* It satisfies every structural rule, which is the point: only
     * being a known placeholder stops it. A test that used a
     * malformed placeholder would prove nothing. */
    TEST_ASSERT_EQUAL(CRED_PLACEHOLDER,
                      cred_check_bot_token("000000000:CHANGE_ME_CHANGE_ME_CHANGE_ME_CHANG"));
}

TEST_CASE("nothing set is reported as such", "[cred_tg]")
{
    TEST_ASSERT_EQUAL(CRED_MISSING, cred_check_bot_token(NULL));
    TEST_ASSERT_EQUAL(CRED_MISSING, cred_check_bot_token(""));
}

TEST_CASE("the secret half alone is refused", "[cred_tg]")
{
    /* The commonest way this goes wrong: the id is dropped on the way
     * through a chat window, and what is left looks plausible. */
    TEST_ASSERT_EQUAL(CRED_BAD_SHAPE,
                      cred_check_bot_token("AAHdqTcvCH1vGWJxfSeofSAs0K5PALDs"));
}

TEST_CASE("a missing or doubled colon is refused", "[cred_tg]")
{
    TEST_ASSERT_EQUAL(CRED_BAD_SHAPE,
                      cred_check_bot_token("123456789AAHdqTcvCH1vGWJxfSeofSAs0K5PALDs"));
    TEST_ASSERT_EQUAL(CRED_BAD_SHAPE,
                      cred_check_bot_token("123456789::AHdqTcvCH1vGWJxfSeofSAs0K5PALD"));
    TEST_ASSERT_EQUAL(CRED_BAD_SHAPE,
                      cred_check_bot_token(":123456789AAHdqTcvCH1vGWJxfSeofSAs0K5PAL"));
}

TEST_CASE("a non-numeric id is refused", "[cred_tg]")
{
    TEST_ASSERT_EQUAL(CRED_BAD_SHAPE,
                      cred_check_bot_token("12345678a:AAHdqTcvCH1vGWJxfSeofSAs0K5PALDs"));
    TEST_ASSERT_EQUAL(CRED_BAD_SHAPE,
                      cred_check_bot_token("bot123456:AAHdqTcvCH1vGWJxfSeofSAs0K5PALDs"));
}

TEST_CASE("an id of implausible length is refused", "[cred_tg]")
{
    TEST_ASSERT_EQUAL(CRED_BAD_SHAPE,
                      cred_check_bot_token("1:AAHdqTcvCH1vGWJxfSeofSAs0K5PALDsAAHd"));
    TEST_ASSERT_EQUAL(
        CRED_BAD_SHAPE,
        cred_check_bot_token("12345678901234567:AAHdqTcvCH1vGWJxfSeofSAs0K5PALDs"));
}

TEST_CASE("a truncated secret is refused", "[cred_tg]")
{
    TEST_ASSERT_EQUAL(CRED_TOO_SHORT, cred_check_bot_token("123456789:AAHdqTcv"));
}

TEST_CASE("an absurdly long secret is refused", "[cred_tg]")
{
    TEST_ASSERT_EQUAL(
        CRED_TOO_LONG,
        cred_check_bot_token("123456789:AAHdqTcvCH1vGWJxfSeofSAs0K5PALDsAAHdqTcvCH1vGWJxfS"
                             "eofSAs0K5PALDsAAHdqTcvCH1vGWJxfSeofSAs0K5PALDs"));
}

TEST_CASE("whitespace in the secret is refused", "[cred_tg]")
{
    /* A token copied out of a chat window arrives with a trailing
     * newline, goes into the URL, and every request comes back 404 -
     * which looks like a revoked token and is not one. */
    TEST_ASSERT_EQUAL(CRED_BAD_SHAPE,
                      cred_check_bot_token("123456789:AAHdqTcvCH1vGWJxfSeofSAs0K5PALD\n"));
    TEST_ASSERT_EQUAL(CRED_BAD_SHAPE,
                      cred_check_bot_token("123456789:AAHdqTcv CH1vGWJxfSeofSAs0K5PALD"));
    TEST_ASSERT_EQUAL(CRED_BAD_SHAPE,
                      cred_check_bot_token("123456789:AAHdqTcvCH1vGWJxfSeofSAs0K5PALD\r"));
}

TEST_CASE("underscores and hyphens are allowed in the secret", "[cred_tg]")
{
    /* Telegram's secret half is base64url, so these are legitimate and
     * refusing them would reject working tokens. */
    TEST_ASSERT_EQUAL(CRED_OK,
                      cred_check_bot_token("123456789:AAHdq_TcvCH1v-GWJxfSeofSAs0K5PAL"));
}

TEST_CASE("a mashed secret is refused", "[cred_tg]")
{
    TEST_ASSERT_EQUAL(CRED_NOT_RANDOM,
                      cred_check_bot_token("123456789:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
}

TEST_CASE("the bounds are loose enough to survive Telegram changing them", "[cred_tg]")
{
    /* These rules exist to catch an unedited placeholder, not to
     * encode a format Telegram has never promised. A token one
     * character either side of today's 35 must still work, because
     * refusing a working token switches off a working channel. */
    TEST_ASSERT_EQUAL(CRED_OK,
                      cred_check_bot_token("1234567890:AAHdqTcvCH1vGWJxfSeofSAs0K5PALDsab"));
    TEST_ASSERT_EQUAL(CRED_OK,
                      cred_check_bot_token("12345678:AAHdqTcvCH1vGWJxfSeofSAs0K5PALDsabcd"));
}

TEST_CASE("the new result has something a human can act on", "[cred_tg]")
{
    const char *t = cred_result_text(CRED_BAD_SHAPE);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_TRUE(t[0] != '\0');
    TEST_ASSERT_TRUE(t[0] != '?');
}
