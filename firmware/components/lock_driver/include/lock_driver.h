/*
 * lock_driver - ESP-IDF wiring around lock_logic.
 *
 * Owns one GPIO into an optocoupler driving a logic-level MOSFET, which
 * energises the 12 V solenoid strike (ADR 0003). All the timing lives
 * in lock_logic; this file only reads the clock and writes the pin.
 *
 * HARDWARE PRECONDITION: the MOSFET gate needs a 10k pulldown to GND.
 * ESP32 GPIOs are high-impedance during reset and while the bootloader
 * runs, so without it the gate floats and the coil can be partially
 * energised for the ~200 ms before app_main takes control. A partially
 * open MOSFET dissipates heat and holds the coil at reduced current:
 * the worst of both states.
 */
#ifndef LOCK_DRIVER_H
#define LOCK_DRIVER_H

#include "driver/gpio.h"
#include "esp_err.h"
#include "lock_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gpio_num_t gpio;   /* to the optocoupler input */
    bool active_high;  /* true: high energises the coil */
    uint32_t pulse_ms; /* 0 selects LOCK_DEFAULT_PULSE_MS */
} lock_driver_config_t;

#define LOCK_DRIVER_DEFAULT_CONFIG()                                                          \
    {                                                                                         \
        .gpio = GPIO_NUM_10, .active_high = true, .pulse_ms = LOCK_DEFAULT_PULSE_MS,          \
    }

/* Configures the pin de-energised before enabling the output, so init
 * itself cannot produce a glitch on the coil. */
esp_err_t lock_driver_init(const lock_driver_config_t *cfg);

/* Call on AC_GRANTED. Returns false when refused (pulse in flight or
 * cooldown), which the caller should log but not surface to the guest:
 * from the door it is indistinguishable from a slow lock. */
bool lock_driver_open(void);

/* Call from the main loop. Writes the pin every time rather than only
 * on change: a single corrupted register write cannot leave the coil
 * latched on for more than one loop iteration. */
void lock_driver_tick(void);

/* Diagnostics for the attempt log. */
const lock_logic_t *lock_driver_state(void);

#ifdef __cplusplus
}
#endif

#endif /* LOCK_DRIVER_H */
