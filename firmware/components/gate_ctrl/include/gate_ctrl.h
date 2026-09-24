/*
 * gate_ctrl - the keypress-to-verdict pipeline.
 *
 * Owns the typed characters, and nothing else. Feeds keys to the ui
 * state machine, submits at nine, hashes, asks access_core for the
 * verdict, and tells the caller what to do about it: fire the lock,
 * persist the counter. Pure C99, so the whole path from keypress to
 * "open the door" runs under CI with a fake hash.
 *
 * Security properties, each covered by a test:
 *   - the plaintext is wiped after every outcome: submit, long press,
 *     timeout. It never outlives the entry that produced it.
 *   - a malformed entry is hashed and evaluated like any other, so it
 *     costs an attempt and takes the same time as a wrong code.
 *   - a lifted lockout is reported for persistence, otherwise a reboot
 *     would restore a counter already at the maximum and the next
 *     single mistake would lock the gate again.
 */
#ifndef GATE_CTRL_H
#define GATE_CTRL_H

#include <stdbool.h>
#include <stdint.h>

#include "access_core.h"
#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* sha256(code || device_salt) on the target; a deterministic fake in
 * the tests. `code` is NUL-terminated and exactly AC_CODE_LEN long. */
typedef void (*gate_hash_fn)(const char *code, uint8_t out[AC_HASH_LEN], void *user);

typedef struct {
    ac_ctx_t *ac;
    ui_ctx_t *ui;
    gate_hash_fn hash;
    void *hash_user;
    char buf[AC_CODE_LEN + 1];
} gate_ctrl_t;

typedef struct {
    bool submitted;        /* an entry reached nine characters */
    ac_result_t result;    /* meaningful when submitted */
    bool open_lock;        /* caller fires lock_driver_open() */
    bool persist_attempts; /* counter or lockout changed */
    char matched_id[AC_ID_LEN];
} gate_out_t;

void gate_init(gate_ctrl_t *g, ac_ctx_t *ac, ui_ctx_t *ui, gate_hash_fn hash, void *user);

gate_out_t gate_on_key(gate_ctrl_t *g, char key, ui_now_t now);
gate_out_t gate_on_long_press(gate_ctrl_t *g, ui_now_t now);

/* Drives ac_tick and ui_tick. Call every loop iteration. */
gate_out_t gate_tick(gate_ctrl_t *g, ui_now_t now);

#ifdef __cplusplus
}
#endif

#endif /* GATE_CTRL_H */
