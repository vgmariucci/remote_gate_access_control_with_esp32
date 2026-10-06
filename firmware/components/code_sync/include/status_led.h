/*
 * status_led - one LED, one truth.
 *
 * Pure C99: this decides the colour, the driver blinks it.
 *
 * The OLED stays dark until a keypress (the UI decision), so the LED is
 * where an unattended gate says how it is doing. A short blink every 30
 * seconds is enough to read from across a yard and dim enough not to
 * light the place up at night.
 */
#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LED_BLINK_PERIOD_MS 30000u
#define LED_BLINK_ON_MS 500u

typedef enum {
    LED_GREEN = 0, /* online, codes fresh */
    LED_RED,       /* no network */
    LED_AMBER,     /* online, but the code set is stale */
    LED_BLUE,      /* provisioning portal open */
    LED_MAGENTA,   /* no trusted clock: every code is being refused */
} led_colour_t;

typedef struct {
    bool online;
    bool sync_stale;
    bool portal_open;
    bool clock_trusted;
} led_inputs_t;

/* Most serious wins: one LED can only say one thing, and the thing it
 * should say is whatever would most change what you do next. */
led_colour_t led_colour(const led_inputs_t *in);

/* True while the LED should be lit, given the free-running clock. */
bool led_is_on(uint32_t now_ms);

const char *led_colour_name(led_colour_t c);

#ifdef __cplusplus
}
#endif

#endif /* STATUS_LED_H */