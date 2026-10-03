#include <stdio.h>
/*
 * Unity tests for prov_form.
 *
 * This is the portal's attack surface: every byte here was chosen by
 * whoever is connected to the AP. The cases that matter are the ones
 * where the input is malformed or too long, because the alternatives
 * are reading past a buffer or silently saving a truncated passphrase.
 */
#include <string.h>

#include "prov_form.h"
#include "unity.h"

static char out[64];

TEST_CASE("plain text decodes unchanged", "[form]")
{
    const char *s = "casa-wifi";
    TEST_ASSERT_EQUAL_INT(9, prov_url_decode(out, sizeof(out), s, strlen(s)));
    TEST_ASSERT_EQUAL_STRING("casa-wifi", out);
}

TEST_CASE("plus becomes space and percent escapes decode", "[form]")
{
    const char *s = "minha+rede%20de+casa";
    TEST_ASSERT_TRUE(prov_url_decode(out, sizeof(out), s, strlen(s)) > 0);
    TEST_ASSERT_EQUAL_STRING("minha rede de casa", out);
}

TEST_CASE("escapes that look like syntax decode to bytes", "[form]")
{
    /* A passphrase may contain & = + %, and must survive the round
     * trip intact. */
    const char *s = "a%26b%3Dc%2Bd%25e";
    TEST_ASSERT_TRUE(prov_url_decode(out, sizeof(out), s, strlen(s)) > 0);
    TEST_ASSERT_EQUAL_STRING("a&b=c+d%e", out);
}

TEST_CASE("non-ASCII bytes survive", "[form]")
{
    /* "portão" in UTF-8. The SSID is opaque bytes to us. */
    const char *s = "port%C3%A3o";
    TEST_ASSERT_EQUAL_INT(7, prov_url_decode(out, sizeof(out), s, strlen(s)));
    TEST_ASSERT_EQUAL_UINT8(0xC3, (unsigned char)out[4]);
    TEST_ASSERT_EQUAL_UINT8(0xA3, (unsigned char)out[5]);
}

TEST_CASE("an escape cut short is rejected, not read past", "[form]")
{
    const char *a = "abc%2";
    const char *b = "abc%";
    TEST_ASSERT_EQUAL_INT(-1, prov_url_decode(out, sizeof(out), a, strlen(a)));
    TEST_ASSERT_EQUAL_STRING("", out); /* zeroed, not half-decoded */
    TEST_ASSERT_EQUAL_INT(-1, prov_url_decode(out, sizeof(out), b, strlen(b)));
}

TEST_CASE("a non-hex escape is rejected", "[form]")
{
    const char *s = "abc%zzdef";
    TEST_ASSERT_EQUAL_INT(-1, prov_url_decode(out, sizeof(out), s, strlen(s)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

TEST_CASE("a value that does not fit is refused, never truncated", "[form]")
{
    char small[8];
    const char *s = "0123456789";
    TEST_ASSERT_EQUAL_INT(-1, prov_url_decode(small, sizeof(small), s, strlen(s)));
    TEST_ASSERT_EQUAL_STRING("", small);

    /* Exactly filling the buffer, terminator included, is fine. */
    const char *fits = "0123456";
    TEST_ASSERT_EQUAL_INT(7, prov_url_decode(small, sizeof(small), fits, strlen(fits)));
    TEST_ASSERT_EQUAL_STRING("0123456", small);
}

TEST_CASE("an empty input decodes to an empty string", "[form]")
{
    TEST_ASSERT_EQUAL_INT(0, prov_url_decode(out, sizeof(out), "", 0));
    TEST_ASSERT_EQUAL_STRING("", out);
}

/* ------------------------------------------------------------ fields */

TEST_CASE("fields are found anywhere in the body", "[form]")
{
    const char *body = "ssid=casa&psk=segredo&hidden=1";
    TEST_ASSERT_TRUE(prov_form_field(body, "ssid", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("casa", out);
    TEST_ASSERT_TRUE(prov_form_field(body, "psk", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("segredo", out);
    TEST_ASSERT_TRUE(prov_form_field(body, "hidden", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("1", out);
}

TEST_CASE("a field name must match the whole name", "[form]")
{
    /* The dangerous confusion: reading someone else's field because
     * the name happens to be a substring. */
    TEST_ASSERT_FALSE(prov_form_field("myssid=wrong", "ssid", out, sizeof(out)));
    TEST_ASSERT_FALSE(prov_form_field("ssid2=wrong", "ssid", out, sizeof(out)));

    TEST_ASSERT_TRUE(prov_form_field("ssid2=wrong&ssid=right", "ssid", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("right", out);
    TEST_ASSERT_TRUE(prov_form_field("xssid=no&ssid=yes", "ssid", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("yes", out);
}

TEST_CASE("a missing field is reported, not left stale", "[form]")
{
    strcpy(out, "previous value");
    TEST_ASSERT_FALSE(prov_form_field("psk=x", "ssid", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

TEST_CASE("an empty value is a value", "[form]")
{
    /* An open network posts an empty passphrase; that is different
     * from not sending the field at all. */
    TEST_ASSERT_TRUE(prov_form_field("ssid=casa&psk=", "psk", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_TRUE(prov_form_field("psk=&ssid=casa", "psk", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

TEST_CASE("a value carrying an escaped ampersand stays whole", "[form]")
{
    TEST_ASSERT_TRUE(prov_form_field("psk=a%26b&ssid=casa", "psk", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("a&b", out);
    TEST_ASSERT_TRUE(prov_form_field("psk=a%26b&ssid=casa", "ssid", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("casa", out);
}

TEST_CASE("an oversized value is refused rather than cut", "[form]")
{
    char small[8];
    TEST_ASSERT_FALSE(prov_form_field("psk=0123456789abcdef", "psk", small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("", small);
}

TEST_CASE("a malformed body does not crash", "[form]")
{
    TEST_ASSERT_FALSE(prov_form_field("", "ssid", out, sizeof(out)));
    TEST_ASSERT_FALSE(prov_form_field("&&&", "ssid", out, sizeof(out)));
    TEST_ASSERT_FALSE(prov_form_field("ssid", "ssid", out, sizeof(out))); /* no '=' */
    TEST_ASSERT_FALSE(prov_form_field("=value", "ssid", out, sizeof(out)));
    TEST_ASSERT_FALSE(prov_form_field(NULL, "ssid", out, sizeof(out)));
    TEST_ASSERT_FALSE(prov_form_field("ssid=x", NULL, out, sizeof(out)));
}

TEST_CASE("a 63-character passphrase fits its buffer", "[form]")
{
    char body[128];
    char psk[64];
    char expected[64];
    memset(expected, 'a', 63);
    expected[63] = '\0';
    snprintf(body, sizeof(body), "psk=%s", expected);

    TEST_ASSERT_TRUE(prov_form_field(body, "psk", psk, sizeof(psk)));
    TEST_ASSERT_EQUAL_STRING(expected, psk);
}

/* ------------------------------------------------------- credentials */

TEST_CASE("credential comparison accepts only an exact match", "[form]")
{
    TEST_ASSERT_TRUE(prov_ct_str_equal("hunter2", "hunter2"));
    TEST_ASSERT_FALSE(prov_ct_str_equal("hunter2", "hunter3"));
    TEST_ASSERT_FALSE(prov_ct_str_equal("hunter2", "hunter"));  /* prefix */
    TEST_ASSERT_FALSE(prov_ct_str_equal("hunter", "hunter2"));  /* extension */
    TEST_ASSERT_FALSE(prov_ct_str_equal("Hunter2", "hunter2")); /* case */
    TEST_ASSERT_TRUE(prov_ct_str_equal("", ""));
    TEST_ASSERT_FALSE(prov_ct_str_equal("", "x"));
    TEST_ASSERT_FALSE(prov_ct_str_equal(NULL, "x"));
    TEST_ASSERT_FALSE(prov_ct_str_equal("x", NULL));
}
