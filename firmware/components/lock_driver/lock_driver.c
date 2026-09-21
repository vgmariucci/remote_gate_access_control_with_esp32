#include "lock_driver.h"

#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "lock";

static lock_driver_config_t s_cfg;
static lock_logic_t s_logic;
static bool s_ready;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void write_coil(bool energised)
{
    int level = s_cfg.active_high ? (energised ? 1 : 0) : (energised ? 0 : 1);
    gpio_set_level(s_cfg.gpio, level);
}

esp_err_t lock_driver_init(const lock_driver_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(&s_cfg, cfg, sizeof(s_cfg));
    lock_logic_init(&s_logic, s_cfg.pulse_ms);

    /* Level first, then direction. Setting the level on a pin that is
     * still an input is harmless and guarantees the output latch holds
     * the de-energised value before the driver is enabled. */
    gpio_set_level(s_cfg.gpio, s_cfg.active_high ? 0 : 1);

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << s_cfg.gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE, /* belt and braces; the
                                               * external 10k is what
                                               * actually matters */
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }
    write_coil(false);

    s_ready = true;
    ESP_LOGI(TAG, "gpio %d, pulse %u ms, ceiling %u ms", (int)s_cfg.gpio,
             (unsigned)s_logic.pulse_ms, (unsigned)LOCK_MAX_PULSE_MS);
    return ESP_OK;
}

bool lock_driver_open(void)
{
    if (!s_ready) {
        return false;
    }
    uint32_t t = now_ms();
    bool accepted = lock_logic_request(&s_logic, t);
    write_coil(lock_logic_output(&s_logic, t));
    if (!accepted) {
        ESP_LOGW(TAG, "open refused (pulse or cooldown in flight)");
    }
    return accepted;
}

void lock_driver_tick(void)
{
    if (!s_ready) {
        return;
    }
    uint32_t t = now_ms();
    lock_logic_tick(&s_logic, t);
    write_coil(lock_logic_output(&s_logic, t));
}

const lock_logic_t *lock_driver_state(void)
{
    return &s_logic;
}
