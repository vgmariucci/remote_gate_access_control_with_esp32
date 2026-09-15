#include "keypad.h"

#include "esp_log.h"
#include "esp_rom_sys.h" /* esp_rom_delay_us */
#include "esp_timer.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "keypad";

static keypad_config_t s_cfg;
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static volatile bool s_running;

/*
 * One scan pass: drive each row low in turn, read the columns. A column
 * reads low when its key on the active row is closed, because the inputs
 * are pulled up.
 */
static uint16_t keypad_scan_once(void)
{
    uint16_t bitmap = 0;

    for (int r = 0; r < 4; r++) {
        for (int i = 0; i < 4; i++) {
            gpio_set_level(s_cfg.rows[i], i == r ? 0 : 1);
        }
        /* Let the line settle. Long cable runs to an outdoor keypad need
         * more than you would expect; 50 us is safe up to a few metres. */
        esp_rom_delay_us(50);

        for (int c = 0; c < 4; c++) {
            if (gpio_get_level(s_cfg.cols[c]) == 0) {
                bitmap |= (uint16_t)(1u << (r * 4 + c));
            }
        }
    }

    for (int i = 0; i < 4; i++) {
        gpio_set_level(s_cfg.rows[i], 1);
    }
    return bitmap;
}

static void keypad_task(void *arg)
{
    (void)arg;
    kp_logic_t logic;
    kp_event_t events[KP_MAX_EVENTS];
    TickType_t last_wake = xTaskGetTickCount();

    kp_logic_init(&logic);

    while (s_running) {
        uint16_t raw = keypad_scan_once();
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        size_t n = kp_logic_update(&logic, raw, now_ms, events, KP_MAX_EVENTS);
        for (size_t i = 0; i < n; i++) {
            /* Never log the key itself: a serial console left attached at
             * the gate would otherwise record guest codes in plaintext. */
            ESP_LOGD(TAG, "event type=%d", (int)events[i].type);
            if (xQueueSend(s_queue, &events[i], 0) != pdTRUE) {
                ESP_LOGW(TAG, "event queue full, dropping");
            }
        }

        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(s_cfg.scan_period_ms));
    }

    vTaskDelete(NULL);
}

esp_err_t keypad_start(const keypad_config_t *cfg, QueueHandle_t *out_queue)
{
    if (!cfg || !out_queue) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(&s_cfg, cfg, sizeof(s_cfg));

    gpio_config_t rows_io = {
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
        .pin_bit_mask = 0,
    };
    gpio_config_t cols_io = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
        .pin_bit_mask = 0,
    };
    for (int i = 0; i < 4; i++) {
        rows_io.pin_bit_mask |= (1ULL << s_cfg.rows[i]);
        cols_io.pin_bit_mask |= (1ULL << s_cfg.cols[i]);
    }
    ESP_ERROR_CHECK(gpio_config(&rows_io));
    ESP_ERROR_CHECK(gpio_config(&cols_io));
    for (int i = 0; i < 4; i++) {
        gpio_set_level(s_cfg.rows[i], 1);
    }

    s_queue = xQueueCreate(s_cfg.queue_depth, sizeof(kp_event_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }

    s_running = true;
    if (xTaskCreate(keypad_task, "keypad", 3072, NULL, 6, &s_task) != pdPASS) {
        s_running = false;
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    *out_queue = s_queue;
    ESP_LOGI(TAG, "scanning every %u ms", (unsigned)s_cfg.scan_period_ms);
    return ESP_OK;
}

void keypad_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;
    vTaskDelay(pdMS_TO_TICKS(s_cfg.scan_period_ms * 3));
    if (s_queue) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
    s_task = NULL;
}
