#include "persist_ring.h"

#include <string.h>

/* Byte layout, little-endian, written explicitly so the on-chip format
 * does not depend on compiler struct packing:
 *
 *   0..3   magic
 *   4..7   seq
 *   8      version
 *   9      attempts
 *   10..13 lockout_until
 *   14..17 boot_count
 *   18..19 crc16 over bytes 0..17
 *   20..31 0xFF
 */
#define REC_LEN 20
#define CRC_AT 18

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t crc16(const uint8_t *d, size_t n)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

void pr_encode(uint8_t out[PR_SLOT_SIZE], const pr_state_t *s, uint32_t seq)
{
    memset(out, 0xFF, PR_SLOT_SIZE);
    put32(&out[0], PR_MAGIC);
    put32(&out[4], seq);
    out[8] = PR_VERSION;
    out[9] = s->attempts;
    put32(&out[10], s->lockout_until);
    put32(&out[14], s->boot_count);
    uint16_t c = crc16(out, CRC_AT);
    out[CRC_AT] = (uint8_t)c;
    out[CRC_AT + 1] = (uint8_t)(c >> 8);
}

bool pr_decode(const uint8_t in[PR_SLOT_SIZE], pr_state_t *s, uint32_t *seq)
{
    if (get32(&in[0]) != PR_MAGIC || in[8] != PR_VERSION) {
        return false;
    }
    uint16_t stored = (uint16_t)in[CRC_AT] | ((uint16_t)in[CRC_AT + 1] << 8);
    if (crc16(in, CRC_AT) != stored) {
        return false;
    }
    if (s != NULL) {
        s->attempts = in[9];
        s->lockout_until = get32(&in[10]);
        s->boot_count = get32(&in[14]);
    }
    if (seq != NULL) {
        *seq = get32(&in[4]);
    }
    return true;
}

bool pr_find_latest(const uint8_t *image, size_t slots, pr_state_t *s, pr_cursor_t *cur)
{
    pr_cursor_t best = {.slot = -1, .seq = 0};
    pr_state_t best_state = {0};

    for (size_t i = 0; i < slots; i++) {
        pr_state_t st;
        uint32_t seq;
        if (pr_decode(&image[i * PR_SLOT_SIZE], &st, &seq) &&
            (best.slot < 0 || seq > best.seq)) {
            best.slot = (int)i;
            best.seq = seq;
            best_state = st;
        }
    }

    if (cur != NULL) {
        *cur = best;
    }
    if (best.slot < 0) {
        return false;
    }
    if (s != NULL) {
        *s = best_state;
    }
    return true;
}

int pr_advance(pr_cursor_t *cur, size_t slots)
{
    int next = (cur->slot < 0) ? 0 : (int)((size_t)(cur->slot + 1) % slots);
    cur->slot = next;
    cur->seq += 1;
    return next;
}
