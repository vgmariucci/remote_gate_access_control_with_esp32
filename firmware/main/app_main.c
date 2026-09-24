/*
 * app_main - wiring only.
 *
 * Every decision lives in a tested component: access_core decides,
 * ui decides what to show, gate_ctrl turns keypresses into verdicts,
 * lock_logic protects the coil, oled_screens draws. This file reads
 * the clocks, moves events between them, and touches the hardware.
 */
#include "sdkconfig.h"

/* This project is ESP32-S3 only: the dev console, the pin plan and the
 * USB-Serial/JTAG workflow all depend on it. A fresh sdkconfig can
 * silently pick another chip, so fail loudly instead. */
#if !CONFIG_IDF_TARGET_ESP32S3
#error                                                                                        \
    "Wrong target. This firmware is ESP32-S3 only. Run: unset IDF_TARGET; rm -f sdkconfig && idf.py set-target esp32s3"
#endif

#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "dev_console.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gate_app.h"
#include "gate_ctrl.h"
#include "keypad.h"
#include "lock_driver.h"
#include "mbedtls/sha256.h"
#include "oled_screens.h"
#include "persist_store.h"
#include "rtc_ds3231.h"
#include "ssd1306.h"
#include "ui.h"

static const char *TAG = "app";

#define PIN_I2C_SDA GPIO_NUM_8
#define PIN_I2C_SCL GPIO_NUM_9
#define LOOP_PERIOD_MS 20

static ac_ctx_t s_access;
static ui_ctx_t s_ui;
static gate_ctrl_t s_gate;
static SemaphoreHandle_t s_mutex;
static uint32_t s_boot_count;
static volatile uint32_t s_border_until_ms;
static i2c_master_bus_handle_t s_bus;

/* Replaced by the provisioned salt from encrypted NVS in the portal
 * step. Until then a dev build uses this public constant, and a
 * non-dev build has no salt, so no code can ever match: fail closed. */
static uint8_t s_salt[32];
static size_t s_salt_len;

/* ------------------------------------------------------------------ */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static ui_now_t now_pair(void)
{
    ui_now_t n = {.ms = now_ms(), .epoch = (int64_t)time(NULL)};
    return n;
}

static void hash_code(const char *code, uint8_t out[AC_HASH_LEN], void *user)
{
    (void)user;
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, (const unsigned char *)code, strlen(code));
    mbedtls_sha256_update(&c, s_salt, s_salt_len);
    mbedtls_sha256_finish(&c, out);
    mbedtls_sha256_free(&c);
}

static void persist_attempts(void)
{
    pr_state_t p = {
        .attempts = s_access.failed_attempts,
        .lockout_until = (uint32_t)s_access.lockout_until,
        .boot_count = s_boot_count,
    };
    if (persist_save(&p) != ESP_OK) {
        /* Not fatal: the lock still works. But a power cut now would
         * reset the counter, so it is worth knowing about. */
        ESP_LOGW(TAG, "could not persist attempt counter to EEPROM");
    }
}

static void handle(const gate_out_t *o)
{
    if (o->submitted) {
        /* Result code and slot id only. The typed code never reaches
         * this layer, so it cannot end up in a log. */
        ESP_LOGI(TAG, "attempt: result=%d id=%s", (int)o->result,
                 o->result == AC_GRANTED ? o->matched_id : "-");
    }
    if (o->open_lock && !lock_driver_open()) {
        ESP_LOGW(TAG, "grant ignored: lock pulse or cooldown in flight");
    }
    if (o->persist_attempts) {
        persist_attempts();
    }
}

/* ------------------------------------------------------ gate_app.h */

void gate_app_lock(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}
void gate_app_unlock(void)
{
    xSemaphoreGive(s_mutex);
}
ac_ctx_t *gate_app_access(void)
{
    return &s_access;
}

void gate_app_set_clock_trusted(bool trusted)
{
    s_access.clock_trusted = trusted;
    ui_set_clock_trusted(&s_ui, trusted);
}

void gate_app_hash(const char *code, uint8_t out[AC_HASH_LEN])
{
    hash_code(code, out, NULL);
}

void gate_app_show_border(uint32_t duration_ms)
{
    s_border_until_ms = now_ms() + duration_ms;
}

i2c_master_bus_handle_t gate_app_bus(void)
{
    return s_bus;
}

/* ------------------------------------------------------------ boot */

static bool boot_clock(i2c_master_bus_handle_t bus)
{
    if (rtc_ds3231_init(bus) != ESP_OK) {
        ESP_LOGE(TAG, "RTC init failed: running without trusted time");
        return false;
    }
    int64_t epoch;
    if (rtc_ds3231_get_time(&epoch) != ESP_OK) {
        return false;
    }
    struct timeval tv = {.tv_sec = (time_t)epoch, .tv_usec = 0};
    settimeofday(&tv, NULL);
    return true;
}

static void boot_persist(i2c_master_bus_handle_t bus)
{
    if (persist_init(bus, PERSIST_AT24C32_ADDR) != ESP_OK) {
        /* Fail open on the counter, not on the lock: codes still need a
         * trusted clock and a matching hash. What is lost is protection
         * against brute force across a power cut, so say so loudly. */
        ESP_LOGE(TAG, "EEPROM unavailable: attempt counter will not survive a reboot");
        s_boot_count = 1;
        return;
    }
    pr_state_t p;
    if (persist_load(&p)) {
        ac_restore_attempts(&s_access, p.attempts, (int64_t)p.lockout_until);
        s_boot_count = p.boot_count + 1;
        ESP_LOGI(TAG, "restored %u failed attempts, boot #%u", (unsigned)p.attempts,
                 (unsigned)s_boot_count);
    } else {
        s_boot_count = 1;
    }
    persist_attempts(); /* records the boot count */
}

void app_main(void)
{
    ESP_LOGI(TAG, "portao-firmware starting, heap=%lu",
             (unsigned long)esp_get_free_heap_size());

    s_mutex = xSemaphoreCreateMutex();

#if CONFIG_GATE_DEV_CONSOLE
    static const char DEV_SALT[] = "DEV-ONLY-PUBLIC-SALT";
    s_salt_len = sizeof(DEV_SALT) - 1;
    memcpy(s_salt, DEV_SALT, s_salt_len);
    ESP_LOGW(TAG, "**************************************************************");
    ESP_LOGW(TAG, "* DEVELOPMENT BUILD: dev console and a PUBLIC salt are enabled *");
    ESP_LOGW(TAG, "* Do not install this image on a door.                        *");
    ESP_LOGW(TAG, "**************************************************************");
#else
    s_salt_len = 0;
#endif

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_bus));
    i2c_master_bus_handle_t bus = s_bus;

    ac_init(&s_access, CONFIG_GATE_MAX_FAILED_ATTEMPTS, CONFIG_GATE_LOCKOUT_SECONDS);
    bool clock_ok = boot_clock(bus);
    boot_persist(bus);

    ui_init(&s_ui);
    gate_app_set_clock_trusted(clock_ok);
    gate_init(&s_gate, &s_access, &s_ui, hash_code, NULL);
    ESP_LOGI(TAG, "clock %s", clock_ok ? "trusted" : "UNTRUSTED: all codes refused");

    /* Clear the panel's RAM before turning it off, so the first time
     * it lights it shows our frame rather than power-on noise. */
    ssd1306_config_t oled_cfg = SSD1306_DEFAULT_CONFIG();
    bool oled_ok = (ssd1306_init(bus, &oled_cfg) == ESP_OK);
    static oled_fb_t fb; /* 1 KB: .bss, not the main task stack */
    if (oled_ok) {
        oled_fb_clear(&fb);
        ssd1306_flush(&fb);
        ssd1306_power(false);
    } else {
        ESP_LOGE(TAG, "OLED not responding: the gate works, but blind");
    }

    lock_driver_config_t lock_cfg = LOCK_DRIVER_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(lock_driver_init(&lock_cfg));

    keypad_config_t kp_cfg = KEYPAD_DEFAULT_CONFIG();
    QueueHandle_t keys;
    ESP_ERROR_CHECK(keypad_start(&kp_cfg, &keys));

    dev_console_start();

    /* A hung loop must reboot rather than sit with the coil in an
     * unknown state (ADR 0003). */
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    bool panel_on = false;
    TickType_t last_wake = xTaskGetTickCount();
    /* pdMS_TO_TICKS rounds down; a zero delay asserts in FreeRTOS. */
    TickType_t period = pdMS_TO_TICKS(LOOP_PERIOD_MS);
    if (period == 0) {
        period = 1;
    }

    for (;;) {
        esp_task_wdt_reset();

        gate_app_lock();
        ui_now_t now = now_pair();

        kp_event_t ev;
        while (xQueueReceive(keys, &ev, 0) == pdTRUE) {
            gate_out_t o = (ev.type == KP_EV_LONG_PRESS) ? gate_on_long_press(&s_gate, now)
                                                         : gate_on_key(&s_gate, ev.key, now);
            handle(&o);
        }
        gate_out_t t = gate_tick(&s_gate, now);
        handle(&t);

        lock_driver_tick();
        ui_render_t r = ui_render(&s_ui, &s_access, now);
        gate_app_unlock();

        if (oled_ok) {
            bool want_on = r.panel_on;
            if ((int32_t)(s_border_until_ms - now.ms) > 0) {
                oled_fb_clear(&fb);
                oled_fb_border(&fb);
                want_on = true;
            } else {
                oled_compose(&fb, &r);
            }

            if (want_on) {
                /* Frame first, then power: never light the panel with a
                 * stale frame from the last interaction. */
                if (ssd1306_flush(&fb) != ESP_OK) {
                    ssd1306_invalidate();
                }
                if (!panel_on && ssd1306_power(true) == ESP_OK) {
                    panel_on = true;
                }
            } else if (panel_on && ssd1306_power(false) == ESP_OK) {
                panel_on = false;
            }
        }

        vTaskDelayUntil(&last_wake, period);
    }
}
