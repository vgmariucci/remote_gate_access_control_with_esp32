/*
 * status_led_rgb - the WS2812 on the DevKit, driven from status_led's
 * decision.
 *
 * Target-only: the colour logic is pure and lives in status_led.c, so
 * what is here is RMT and timing.
 */
#ifndef STATUS_LED_RGB_H
#define STATUS_LED_RGB_H

#include "driver/gpio.h"
#include "esp_err.h"
#include "status_led.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t status_led_init(gpio_num_t gpio);

/* Call from the main loop. Decides the colour, blinks it, and writes to
 * the strip only when something actually changed. */
void status_led_tick(const led_inputs_t *in, uint32_t now_ms);

/* Lights a colour immediately, for a console test. The next tick
 * takes the LED back. */
void status_led_force(led_colour_t c);

/* Current colour, for the console. */
led_colour_t status_led_current(void);

#ifdef __cplusplus
}
#endif

#endif /* STATUS_LED_RGB_H */