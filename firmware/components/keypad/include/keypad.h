/*
 * keypad - ESP-IDF driver around keypad_logic.
 *
 * Owns four row outputs and four column inputs, scans them on a periodic
 * task, and posts debounced events to a FreeRTOS queue. All the logic
 * worth testing lives in keypad_logic; this file is just wiring.
 */
#ifndef KEYPAD_H
#define KEYPAD_H

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "keypad_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gpio_num_t rows[4];      /* driven low one at a time */
    gpio_num_t cols[4];      /* read with internal pull-up */
    uint32_t scan_period_ms; /* 5 is comfortable */
    UBaseType_t queue_depth; /* 8 is plenty */
} keypad_config_t;

/* clang-format off */
#define KEYPAD_DEFAULT_CONFIG()                                       \
    {                                                                 \
        .rows = {GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7},     \
        .cols = {GPIO_NUM_15, GPIO_NUM_16, GPIO_NUM_17, GPIO_NUM_18}, \
        .scan_period_ms = 5,                                          \
        .queue_depth = 8,                                             \
    }
/* clang-format on */

/* Starts the scan task. Events arrive as kp_event_t on *out_queue. */
esp_err_t keypad_start(const keypad_config_t *cfg, QueueHandle_t *out_queue);

/* Stops the task and frees the queue. */
void keypad_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* KEYPAD_H */
