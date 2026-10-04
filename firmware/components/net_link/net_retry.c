#include "net_retry.h"

#include <string.h>

void net_retry_init(net_retry_t *r, uint32_t now_ms)
{
    if (r == NULL) {
        return;
    }
    memset(r, 0, sizeof(*r));
    r->delay_ms = NET_RETRY_FIRST_MS;
    r->next_try_ms = now_ms; /* the first attempt waits for nothing */
}

bool net_retry_due(const net_retry_t *r, uint32_t now_ms)
{
    if (r == NULL || r->connected) {
        return false;
    }
    /* Signed difference, so the comparison survives the millisecond
     * counter wrapping. */
    return (int32_t)(now_ms - r->next_try_ms) >= 0;
}

void net_retry_failed(net_retry_t *r, uint32_t now_ms)
{
    if (r == NULL) {
        return;
    }
    r->connected = false;
    if (r->failures < 255) {
        r->failures++;
    }
    r->next_try_ms = now_ms + r->delay_ms;

    uint32_t next = r->delay_ms * 2;
    r->delay_ms = (next > NET_RETRY_MAX_MS || next < r->delay_ms) ? NET_RETRY_MAX_MS : next;
}

void net_retry_connected(net_retry_t *r)
{
    if (r == NULL) {
        return;
    }
    /* A fresh start: the next outage gets the full fast retry again. */
    r->connected = true;
    r->failures = 0;
    r->delay_ms = NET_RETRY_FIRST_MS;
}

void net_retry_disconnected(net_retry_t *r, uint32_t now_ms)
{
    if (r == NULL) {
        return;
    }
    r->connected = false;
    r->failures = 0;
    r->delay_ms = NET_RETRY_FIRST_MS;
    r->next_try_ms = now_ms;
}

bool net_retry_should_log(const net_retry_t *r)
{
    if (r == NULL) {
        return false;
    }
    /* Every one of the first few, then only when the backoff is at its
     * ceiling, so a week offline is a line a minute and not a flood. */
    return r->failures <= NET_RETRY_QUIET_AFTER || r->delay_ms >= NET_RETRY_MAX_MS;
}

uint32_t net_retry_delay_ms(const net_retry_t *r)
{
    return r == NULL ? 0 : r->delay_ms;
}
