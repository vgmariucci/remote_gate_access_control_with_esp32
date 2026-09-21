#include "lock_logic.h"

static bool elapsed(uint32_t now_ms, uint32_t since_ms, uint32_t span_ms)
{
    return (uint32_t)(now_ms - since_ms) >= span_ms;
}

void lock_logic_init(lock_logic_t *st, uint32_t pulse_ms)
{
    if (st == NULL) {
        return;
    }
    if (pulse_ms == 0) {
        pulse_ms = LOCK_DEFAULT_PULSE_MS;
    }
    if (pulse_ms > LOCK_MAX_PULSE_MS) {
        pulse_ms = LOCK_MAX_PULSE_MS;
    }
    st->state = LOCK_IDLE;
    st->pulse_ms = pulse_ms;
    st->pulse_started_ms = 0;
    st->cooldown_until_ms = 0;
    st->pulses_total = 0;
    st->requests_refused = 0;
}

bool lock_logic_request(lock_logic_t *st, uint32_t now_ms)
{
    if (st == NULL) {
        return false;
    }
    lock_logic_tick(st, now_ms);

    if (st->state != LOCK_IDLE) {
        /* Refused, not queued. A queued request would fire the coil
         * again the instant the cooldown lifted, with no one at the
         * door — worse than making the guest press again. */
        st->requests_refused++;
        return false;
    }

    st->state = LOCK_PULSING;
    st->pulse_started_ms = now_ms;
    st->pulses_total++;
    return true;
}

void lock_logic_tick(lock_logic_t *st, uint32_t now_ms)
{
    if (st == NULL) {
        return;
    }
    switch (st->state) {
    case LOCK_PULSING:
        if (elapsed(now_ms, st->pulse_started_ms, st->pulse_ms)) {
            st->state = LOCK_COOLDOWN;
            st->cooldown_until_ms = st->pulse_started_ms + st->pulse_ms + LOCK_COOLDOWN_MS;
        }
        break;
    case LOCK_COOLDOWN:
        if ((int32_t)(st->cooldown_until_ms - now_ms) <= 0) {
            st->state = LOCK_IDLE;
        }
        break;
    case LOCK_IDLE:
    default:
        break;
    }
}

bool lock_logic_output(const lock_logic_t *st, uint32_t now_ms)
{
    if (st == NULL || st->state != LOCK_PULSING) {
        return false;
    }
    /* Evaluated here as well as in tick(). If the main loop stalls -
     * a long flash write, a blocked task - the output still reads
     * false past the ceiling. The coil is protected even when nothing
     * is driving the state machine. */
    return !elapsed(now_ms, st->pulse_started_ms, st->pulse_ms);
}
