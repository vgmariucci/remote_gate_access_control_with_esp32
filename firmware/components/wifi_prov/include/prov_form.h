/*
 * prov_form - parsing what the browser posts.
 *
 * Pure C99. This is the only code in the portal that reads bytes an
 * attacker chooses, so it is the part most worth testing: a percent
 * escape truncated at the end of a buffer, a field name that is a
 * prefix of another, a value longer than the destination.
 *
 * Everything writes into a caller-supplied buffer with an explicit
 * size and refuses rather than truncating. A truncated SSID or
 * passphrase would be saved, fail to connect, and look like a typo.
 */
#ifndef PROV_FORM_H
#define PROV_FORM_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Percent-decodes `src_len` bytes of application/x-www-form-urlencoded
 * text into `dst`, turning '+' into a space and %XX into a byte.
 *
 * Returns the decoded length, or -1 when the result would not fit or
 * the input holds a malformed escape. On failure `dst` is zeroed: a
 * caller that ignores the return value gets an empty string rather
 * than half a passphrase. On success `dst` is NUL-terminated. */
int prov_url_decode(char *dst, size_t dst_size, const char *src, size_t src_len);

/* Finds `name` in a urlencoded body and decodes its value into `out`.
 *
 * The name must match a whole field: "ssid" does not match "ssid2" or
 * "myssid". A field present with an empty value returns true and an
 * empty string. Returns false when the field is absent, the value does
 * not fit, or an escape is malformed. */
bool prov_form_field(const char *body, const char *name, char *out, size_t out_size);

/* Constant-time string comparison, for credentials. Runs over the
 * longer of the two so a wrong password does not return faster than a
 * nearly-right one. */
bool prov_ct_str_equal(const char *a, const char *b);

#ifdef __cplusplus
}
#endif

#endif /* PROV_FORM_H */
