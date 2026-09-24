#include "gate_ctrl.h"

#include <string.h>

/* memset through a volatile pointer so the compiler cannot drop the
 * write as dead: the buffer is never read again, which is exactly the
 * condition under which an optimiser removes a plain memset. */
static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) {
        *v++ = 0;
    }
}

static bool attempts_changed(const ac_ctx_t *ac, uint8_t f0, int64_t l0)
{
    return ac->failed_attempts != f0 || ac->lockout_until != l0;
}

void gate_init(gate_ctrl_t *g, ac_ctx_t *ac, ui_ctx_t *ui, gate_hash_fn hash, void *user)
{
    if (g == NULL) {
        return;
    }
    memset(g, 0, sizeof(*g));
    g->ac = ac;
    g->ui = ui;
    g->hash = hash;
    g->hash_user = user;
}

static void submit(gate_ctrl_t *g, ui_now_t now, gate_out_t *out)
{
    uint8_t h[AC_HASH_LEN];
    uint8_t f0 = g->ac->failed_attempts;
    int64_t l0 = g->ac->lockout_until;

    g->buf[AC_CODE_LEN] = '\0';
    /* No format pre-check here. A malformed entry is hashed and
     * evaluated like any other: it cannot match a stored code, so it
     * lands as AC_DENIED_UNKNOWN and is counted, and it takes the same
     * time as a well-formed wrong guess. ac_format_valid() belongs to
     * the code generator and the dev console, not to this path. */
    g->hash(g->buf, h, g->hash_user);
    wipe(g->buf, sizeof(g->buf));

    ac_result_t r = ac_evaluate(g->ac, h, now.epoch, out->matched_id);
    wipe(h, sizeof(h));

    ui_on_result(g->ui, g->ac, r, now);

    out->submitted = true;
    out->result = r;
    out->open_lock = (r == AC_GRANTED);
    out->persist_attempts = attempts_changed(g->ac, f0, l0);
}

gate_out_t gate_on_key(gate_ctrl_t *g, char key, ui_now_t now)
{
    gate_out_t out;
    memset(&out, 0, sizeof(out));
    if (g == NULL) {
        return out;
    }

    ui_action_t a = ui_on_key(g->ui, g->ac, now);

    /* The ui's entry length is the source of truth. Zero means the key
     * was refused (no clock, lockout); one means a fresh entry started,
     * so any stale characters go first. */
    uint8_t n = g->ui->entry_len;
    if (n == 0) {
        return out;
    }
    if (n == 1) {
        wipe(g->buf, sizeof(g->buf));
    }
    if (n <= AC_CODE_LEN) {
        g->buf[n - 1] = key;
    }

    if (a == UI_ACTION_SUBMIT) {
        submit(g, now, &out);
    }
    return out;
}

gate_out_t gate_on_long_press(gate_ctrl_t *g, ui_now_t now)
{
    gate_out_t out;
    memset(&out, 0, sizeof(out));
    if (g == NULL) {
        return out;
    }
    if (ui_on_long_press(g->ui, g->ac, now) == UI_ACTION_CLEAR_BUFFER) {
        wipe(g->buf, sizeof(g->buf));
    }
    return out;
}

gate_out_t gate_tick(gate_ctrl_t *g, ui_now_t now)
{
    gate_out_t out;
    memset(&out, 0, sizeof(out));
    if (g == NULL) {
        return out;
    }
    uint8_t f0 = g->ac->failed_attempts;
    int64_t l0 = g->ac->lockout_until;

    if (g->ui->clock_trusted) {
        ac_tick(g->ac, now.epoch);
    }
    if (ui_tick(g->ui, g->ac, now) == UI_ACTION_CLEAR_BUFFER) {
        wipe(g->buf, sizeof(g->buf));
    }

    out.persist_attempts = attempts_changed(g->ac, f0, l0);
    return out;
}
