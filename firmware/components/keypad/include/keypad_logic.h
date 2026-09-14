/*
 * keypad_logic - debounce and event extraction for a 4x4 matrix keypad.
 *
 * Pure C99. No GPIO, no FreeRTOS, no time source of its own. The caller
 * feeds it a raw 16-bit "which keys appear pressed" bitmap and a
 * millisecond timestamp; it returns debounced events. That separation is
 * what lets the tricky part (bounce, long-press, ghosting) run under CI
 * with no board attached.
 *
 * Bit layout: bit (row * 4 + col), row 0 = top.
 */
#ifndef KEYPAD_LOGIC_H
#define KEYPAD_LOGIC_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KP_KEYS          16
#define KP_DEBOUNCE_MS   15   /* membrane keypads settle in <10 ms */
#define KP_LONGPRESS_MS  1500 /* long press clears the entry buffer */
#define KP_MAX_EVENTS    4

typedef enum {
    KP_EV_PRESS,      /* debounced key down */
    KP_EV_LONG_PRESS  /* still held after KP_LONGPRESS_MS; fires once */
} kp_event_type_t;

typedef struct {
    kp_event_type_t type;
    char            key; /* '0'-'9', 'A'-'D', '*', '#' */
} kp_event_t;

typedef struct {
    uint16_t stable;              /* last accepted bitmap */
    uint16_t candidate;           /* bitmap awaiting KP_DEBOUNCE_MS */
    uint32_t candidate_since_ms;
    bool     candidate_pending;

    uint32_t press_started_ms;    /* for the single held key */
    int8_t   held_index;          /* -1 when nothing is held */
    bool     long_fired;

    bool     ghost_blocked;       /* >1 key down: ignore until release */
} kp_logic_t;

/* Maps a bit index to its character. Returns 0 for an out-of-range index. */
char kp_index_to_char(int index);

void kp_logic_init(kp_logic_t *st);

/*
 * Feed one scan. `raw` is the bitmap read from the matrix this cycle,
 * `now_ms` a monotonic millisecond clock. Writes up to KP_MAX_EVENTS
 * events into `out` and returns how many were written.
 *
 * Call this at a steady period (5 ms is comfortable). Nothing here
 * allocates or blocks.
 */
size_t kp_logic_update(kp_logic_t *st, uint16_t raw, uint32_t now_ms,
                       kp_event_t *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* KEYPAD_LOGIC_H */
