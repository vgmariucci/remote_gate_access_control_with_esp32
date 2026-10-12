/*
 * cred_check - are these secrets fit for a device on a door?
 *
 * Pure C99. Every credential in this project started life as a
 * placeholder so the bench could run, and placeholders are how IoT
 * devices get taken over: the example value is in the repository, in
 * the README, and in everyone's muscle memory.
 *
 * So a production build asks these questions before it will run, and a
 * development build asks them anyway and complains loudly.
 *
 * The rules are here, pure and tested, rather than in a tangle of
 * #ifdefs: a rule nobody can read is a rule nobody can check.
 */
#ifndef CRED_CHECK_H
#define CRED_CHECK_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An admin password shorter than this is not worth the typing. The
 * portal allows five attempts per session and the session needs the
 * button, so the threat is not online guessing - it is somebody who
 * already knows the usual defaults. */
#define CRED_ADMIN_MIN_LEN 12

/* 802.11 limits. */
#define CRED_AP_MIN_LEN 8
#define CRED_AP_MAX_LEN 63

/* A salt shorter than this adds little to a 9-character code. */
#define CRED_SALT_MIN_LEN 16

#define CRED_KEY_HEX_LEN 64

typedef enum {
    CRED_OK = 0,
    CRED_MISSING,     /* NULL or empty */
    CRED_TOO_SHORT,   /* below the minimum for its job */
    CRED_TOO_LONG,    /* above what the protocol allows */
    CRED_PLACEHOLDER, /* still the value shipped in the example file */
    CRED_WEAK,        /* on any password list ever compiled */
    CRED_NOT_HEX,     /* a key that is not 64 lowercase hex characters */
    CRED_NOT_RANDOM,  /* all one character: somebody mashed a key */
} cred_result_t;

cred_result_t cred_check_admin_password(const char *password);
cred_result_t cred_check_ap_password(const char *password);
cred_result_t cred_check_delivery_key(const char *hex);
cred_result_t cred_check_device_salt(const char *salt);

/* The gate id and URL are not secrets, but an unedited placeholder
 * means the file was never filled in at all. */
cred_result_t cred_check_identity(const char *gate_id, const char *url);

const char *cred_result_text(cred_result_t r);

#ifdef __cplusplus
}
#endif

#endif /* CRED_CHECK_H */
