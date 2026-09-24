/*
 * persist_ring - wear-levelled, torn-write-safe storage for the attempt
 * counter (ADR 0004, as amended for the DS3231M).
 *
 * Pure C99. Operates on byte images of EEPROM slots, so every property
 * below is tested on the host with no chip attached.
 *
 * Layout: PR_SLOTS slots of PR_SLOT_SIZE bytes, one AT24C32 page each.
 * Each save goes to the slot after the newest one, carrying a sequence
 * number one higher. Load picks the valid record with the highest
 * sequence number.
 *
 *   - Wear: writes rotate across all 128 pages, so the ~1M write
 *     endurance of one page becomes ~128M for the store.
 *   - Torn writes: a record interrupted by a power cut fails its CRC and
 *     is skipped, so load falls back to the previous record. A partial
 *     write can never yield a corrupted counter.
 *   - Blank chip: all 0xFF fails the magic check; load reports nothing
 *     found and the caller starts from zero.
 */
#ifndef PERSIST_RING_H
#define PERSIST_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PR_SLOT_SIZE 32      /* one AT24C32 page: a slot write never straddles pages */
#define PR_SLOTS 128         /* 4096 / 32 */
#define PR_MAGIC 0x47415445u /* "GATE" */
#define PR_VERSION 1

typedef struct {
    uint8_t attempts;
    uint32_t lockout_until; /* unix epoch; 0 = not locked out */
    uint32_t boot_count;
} pr_state_t;

typedef struct {
    int slot;     /* index of the newest valid record, -1 when none */
    uint32_t seq; /* its sequence number, 0 when none */
} pr_cursor_t;

/* Fills a slot image. Unused bytes are 0xFF, matching an erased chip. */
void pr_encode(uint8_t out[PR_SLOT_SIZE], const pr_state_t *s, uint32_t seq);

/* False on bad magic, unknown version or CRC mismatch. */
bool pr_decode(const uint8_t in[PR_SLOT_SIZE], pr_state_t *s, uint32_t *seq);

/* Scans `slots` consecutive slot images. Returns true and fills *s and
 * *cur when at least one valid record exists. */
bool pr_find_latest(const uint8_t *image, size_t slots, pr_state_t *s, pr_cursor_t *cur);

/* Where the next save goes, and advances the cursor to it. */
int pr_advance(pr_cursor_t *cur, size_t slots);

#ifdef __cplusplus
}
#endif

#endif /* PERSIST_RING_H */
