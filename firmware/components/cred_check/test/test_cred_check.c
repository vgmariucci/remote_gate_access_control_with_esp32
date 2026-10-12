/*
 * Unity tests for cred_check.
 *
 * The case that matters is the one that actually happens: the device
 * ships with whatever was in admin_credentials.h on the day somebody
 * stopped testing and screwed the enclosure shut.
 */
#include <string.h>

#include "cred_check.h"
#include "unity.h"

TEST_CASE("the value we have been using all along is refused", "[cred]")
{
    /* admin123 has been both the portal password and the AP passphrase
     * on the bench since the portal was written. */
    TEST_ASSERT_EQUAL_INT(CRED_PLACEHOLDER, cred_check_admin_password("admin123"));
    TEST_ASSERT_EQUAL_INT(CRED_PLACEHOLDER, cred_check_ap_password("admin123"));
}

TEST_CASE("the example file's own values are refused", "[cred]")
{
    TEST_ASSERT_EQUAL_INT(CRED_PLACEHOLDER, cred_check_admin_password("CHANGE_ME"));
    TEST_ASSERT_EQUAL_INT(CRED_PLACEHOLDER, cred_check_device_salt("DEV_SALT"));
    TEST_ASSERT_EQUAL_INT(
        CRED_PLACEHOLDER,
        cred_check_delivery_key(
            "0000000000000000000000000000000000000000000000000000000000000000"));
}

TEST_CASE("a good set passes", "[cred]")
{
    TEST_ASSERT_EQUAL_INT(CRED_OK, cred_check_admin_password("correct horse battery"));
    TEST_ASSERT_EQUAL_INT(CRED_OK, cred_check_ap_password("um-portao-no-quintal"));
    TEST_ASSERT_EQUAL_INT(CRED_OK, cred_check_device_salt("JkQ2mXp9rLtVnB4z"));
    TEST_ASSERT_EQUAL_INT(
        CRED_OK, cred_check_delivery_key(
                     "3c7beb35e0f225293710d8074ae6050ffe52879224e5185a23b3e7b9d557c4e6"));
}

TEST_CASE("an admin password must be long enough to be worth typing", "[cred]")
{
    TEST_ASSERT_EQUAL_INT(CRED_TOO_SHORT, cred_check_admin_password("short1!"));
    TEST_ASSERT_EQUAL_INT(CRED_TOO_SHORT, cred_check_admin_password("elevenchar"));
    TEST_ASSERT_EQUAL_INT(CRED_OK, cred_check_admin_password("twelvechars1"));
}

TEST_CASE("the AP passphrase respects the radio's limits", "[cred]")
{
    char too_long[70];
    memset(too_long, 'x', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';

    TEST_ASSERT_EQUAL_INT(CRED_TOO_SHORT, cred_check_ap_password("1234567"));
    TEST_ASSERT_EQUAL_INT(CRED_OK, cred_check_ap_password("12345678x"));
    TEST_ASSERT_EQUAL_INT(CRED_TOO_LONG, cred_check_ap_password(too_long));
}

TEST_CASE("a weak password is refused however long it is", "[cred]")
{
    TEST_ASSERT_EQUAL_INT(CRED_WEAK, cred_check_ap_password("portao123"));
    TEST_ASSERT_EQUAL_INT(CRED_WEAK, cred_check_ap_password("qwerty123"));
}

TEST_CASE("a key mashed from one character is refused", "[cred]")
{
    /* 64 valid hex characters, and no entropy at all. */
    TEST_ASSERT_EQUAL_INT(
        CRED_NOT_RANDOM,
        cred_check_delivery_key(
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    TEST_ASSERT_EQUAL_INT(CRED_NOT_RANDOM, cred_check_device_salt("ssssssssssssssss"));
}

TEST_CASE("a key of the wrong shape is refused", "[cred]")
{
    TEST_ASSERT_EQUAL_INT(CRED_NOT_HEX, cred_check_delivery_key("deadbeef"));
    /* Uppercase verifies nothing: the generator writes lowercase. */
    TEST_ASSERT_EQUAL_INT(
        CRED_NOT_HEX, cred_check_delivery_key(
                          "3C7BEB35E0F225293710D8074AE6050FFE52879224E5185A23B3E7B9D557C4E6"));
    TEST_ASSERT_EQUAL_INT(
        CRED_NOT_HEX, cred_check_delivery_key(
                          "zzzbeb35e0f225293710d8074ae6050ffe52879224e5185a23b3e7b9d557c4e6"));
}

TEST_CASE("a short salt adds little to a nine-character code", "[cred]")
{
    TEST_ASSERT_EQUAL_INT(CRED_TOO_SHORT, cred_check_device_salt("fifteenchars123"));
    TEST_ASSERT_EQUAL_INT(CRED_OK, cred_check_device_salt("sixteenchars1234"));
}

TEST_CASE("nothing set at all is reported as such", "[cred]")
{
    TEST_ASSERT_EQUAL_INT(CRED_MISSING, cred_check_admin_password(NULL));
    TEST_ASSERT_EQUAL_INT(CRED_MISSING, cred_check_admin_password(""));
    TEST_ASSERT_EQUAL_INT(CRED_MISSING, cred_check_delivery_key(NULL));
    TEST_ASSERT_EQUAL_INT(CRED_MISSING, cred_check_device_salt(""));
}

TEST_CASE("the sync URL must be https", "[cred]")
{
    /* Signed either way, but plain HTTP puts the code hashes on the
     * wire for anyone on the path. */
    TEST_ASSERT_EQUAL_INT(CRED_WEAK,
                          cred_check_identity("portao-01", "http://example.net/gate.txt"));
    TEST_ASSERT_EQUAL_INT(CRED_OK,
                          cred_check_identity("portao-01", "https://example.net/gate.txt"));
}

TEST_CASE("an unedited identity means the file was never filled in", "[cred]")
{
    TEST_ASSERT_EQUAL_INT(CRED_PLACEHOLDER,
                          cred_check_identity("portao-01-placeholder", "https://x.net/g.txt"));
    TEST_ASSERT_EQUAL_INT(CRED_PLACEHOLDER,
                          cred_check_identity("portao-01", "https://example.com/gate.txt"));
    TEST_ASSERT_EQUAL_INT(CRED_MISSING, cred_check_identity("", "https://x.net/g.txt"));
}

TEST_CASE("every result has something a human can act on", "[cred]")
{
    /* A refusal nobody understands gets worked around rather than
     * fixed. */
    const cred_result_t all[] = {CRED_OK,   CRED_MISSING,     CRED_TOO_SHORT, CRED_TOO_LONG,
                                 CRED_WEAK, CRED_PLACEHOLDER, CRED_NOT_HEX,   CRED_NOT_RANDOM};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        const char *t = cred_result_text(all[i]);
        TEST_ASSERT_NOT_NULL(t);
        TEST_ASSERT_TRUE(strlen(t) > 1);
        TEST_ASSERT_TRUE(strcmp(t, "?") != 0);
    }
}
