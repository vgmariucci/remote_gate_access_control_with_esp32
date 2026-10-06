#include "status_led.h"

led_colour_t led_colour(const led_inputs_t *in)
{
    if (in == NULL) {
        return LED_RED;
    }
    /* A gate refusing every code is the worst thing that can be true,
     * and it is invisible otherwise: the guest is stuck at the door
     * through no fault of their own (ADR 0002 fails closed). */
    if (!in->clock_trusted) {
        return LED_MAGENTA;
    }
    /* Next: a configuration radio is live. Nobody should walk away
     * from that by accident. */
    if (in->portal_open) {
        return LED_BLUE;
    }
    if (!in->online) {
        return LED_RED;
    }
    /* The quiet failure: the link is fine, so red would be a lie, but
     * the codes may be out of date. */
    if (in->sync_stale) {
        return LED_AMBER;
    }
    return LED_GREEN;
}

bool led_is_on(uint32_t now_ms)
{
    return (now_ms % LED_BLINK_PERIOD_MS) < LED_BLINK_ON_MS;
}

const char *led_colour_name(led_colour_t c)
{
    switch (c) {
    case LED_GREEN:
        return "green (online)";
    case LED_RED:
        return "red (offline)";
    case LED_AMBER:
        return "amber (codes stale)";
    case LED_BLUE:
        return "blue (portal open)";
    case LED_MAGENTA:
        return "magenta (no trusted clock)";
    default:
        return "?";
    }
}
