#include "keypad_logic.h"

/* clang-format off */
static const char KP_MAP[KP_KEYS] = {
    '1', '2', '3', 'A', '4', '5', '6', 'B', '7', '8', '9', 'C', '*', '0', '#', 'D',
};
/* clang-format on */

char kp_index_to_char(int index)
{
    if (index < 0 || index >= KP_KEYS) {
        return 0;
    }
    return KP_MAP[index];
}

static int popcount16(uint16_t v)
{
    int n = 0;
    while (v) {
        v &= (uint16_t)(v - 1);
        n++;
    }
    return n;
}

static int lowest_set_index(uint16_t v)
{
    for (int i = 0; i < KP_KEYS; i++) {
        if (v & (uint16_t)(1u << i)) {
            return i;
        }
    }
    return -1;
}

void kp_logic_init(kp_logic_t *st)
{
    if (!st) {
        return;
    }
    st->stable = 0;
    st->candidate = 0;
    st->candidate_since_ms = 0;
    st->candidate_pending = false;
    st->press_started_ms = 0;
    st->held_index = -1;
    st->long_fired = false;
    st->ghost_blocked = false;
}

size_t kp_logic_update(kp_logic_t *st, uint16_t raw, uint32_t now_ms, kp_event_t *out,
                       size_t out_cap)
{
    size_t n = 0;
    if (!st || !out || out_cap == 0) {
        return 0;
    }

    /* Ghosting: a diode-less matrix reports phantom keys when several are
     * held. Refuse to interpret anything until the user lets go. */
    if (popcount16(raw) > 1) {
        st->ghost_blocked = true;
    } else if (raw == 0) {
        st->ghost_blocked = false;
    }
    if (st->ghost_blocked) {
        raw = 0;
    }

    /* Debounce: a bitmap must hold steady for KP_DEBOUNCE_MS to be taken. */
    if (raw != st->stable) {
        if (!st->candidate_pending || raw != st->candidate) {
            st->candidate = raw;
            st->candidate_since_ms = now_ms;
            st->candidate_pending = true;
        } else if ((uint32_t)(now_ms - st->candidate_since_ms) >= KP_DEBOUNCE_MS) {
            uint16_t previous = st->stable;
            st->stable = st->candidate;
            st->candidate_pending = false;

            uint16_t newly_down = (uint16_t)(st->stable & ~previous);
            if (newly_down) {
                int idx = lowest_set_index(newly_down);
                st->held_index = (int8_t)idx;
                st->press_started_ms = now_ms;
                st->long_fired = false;
                if (n < out_cap) {
                    out[n].type = KP_EV_PRESS;
                    out[n].key = kp_index_to_char(idx);
                    n++;
                }
            }
            if (st->stable == 0) {
                st->held_index = -1;
                st->long_fired = false;
            }
        }
    } else {
        st->candidate_pending = false;
    }

    /* Long press on the key currently held. Fires once. */
    if (st->held_index >= 0 && !st->long_fired &&
        (uint32_t)(now_ms - st->press_started_ms) >= KP_LONGPRESS_MS) {
        st->long_fired = true;
        if (n < out_cap) {
            out[n].type = KP_EV_LONG_PRESS;
            out[n].key = kp_index_to_char(st->held_index);
            n++;
        }
    }

    return n;
}
