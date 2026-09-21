/*
 * lock_logic - pulse timing and coil protection for the solenoid.
 *
 * Pure C99, no GPIO. The caller ticks it with a monotonic millisecond
 * clock and asks what the output level should be.
 *
 * The HDL strike draws 1.5-2 A of inrush and is rated for momentary
 * energisation only; holding it on burns the coil (ADR 0003). That
 * makes "never energised longer than the ceiling" a safety property
 * rather than a nicety, so it lives here where it can be tested
 * exhaustively instead of in an interrupt handler on the board.
 *
 * Three guarantees:
 *   1. The output is de-energised after LOCK_MAX_PULSE_MS, even if the
 *      caller stops ticking and resumes minutes later.
 *   2. A request arriving mid-pulse is refused, never merged. Two
 *      grants in quick succession cannot become one long pulse.
 *   3. A cooldown follows every pulse, so a rapid sequence of valid
 *      codes cannot cook the coil by duty cycle.
 */
#ifndef LOCK_LOGIC_H
#define LOCK_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOCK_DEFAULT_PULSE_MS 800
#define LOCK_MAX_PULSE_MS 1000 /* absolute ceiling; nothing exceeds it */
#define LOCK_COOLDOWN_MS 3000  /* thermal margin between pulses */

typedef enum {
    LOCK_IDLE,    /* de-energised, ready */
    LOCK_PULSING, /* energised */
    LOCK_COOLDOWN /* de-energised, refusing requests */
} lock_state_t;

typedef struct {
    lock_state_t state;
    uint32_t pulse_ms; /* clamped at init */
    uint32_t pulse_started_ms;
    uint32_t cooldown_until_ms;
    uint32_t pulses_total; /* diagnostics / attempt log */
    uint32_t requests_refused;
} lock_logic_t;

/* pulse_ms is clamped to [1, LOCK_MAX_PULSE_MS]. Passing 0 selects the
 * default rather than a zero-length pulse, so a miswired config cannot
 * silently produce a lock that never opens. */
void lock_logic_init(lock_logic_t *st, uint32_t pulse_ms);

/* Returns true when the pulse was started. False means refused: either
 * a pulse is already running or the cooldown has not elapsed. */
bool lock_logic_request(lock_logic_t *st, uint32_t now_ms);

/* Drives the timeouts. Safe to call at any interval, including never
 * for a while: the ceiling is evaluated against now_ms, not against
 * the number of ticks. */
void lock_logic_tick(lock_logic_t *st, uint32_t now_ms);

/* True when the coil should be energised right now. This is the only
 * function the GPIO layer needs. */
bool lock_logic_output(const lock_logic_t *st, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* LOCK_LOGIC_H */
