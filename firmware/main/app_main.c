/*
 * Milestone 0/1 entry point.
 *
 * Does nothing but prove the toolchain, the partition layout and the
 * access_core component all link together. Every later milestone
 * replaces a TODO here with a real module.
 */
#include "access_core.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "app";

#define MAX_FAILED_ATTEMPTS 5
#define LOCKOUT_SECONDS 60

static ac_ctx_t s_access;

void app_main(void)
{
    ESP_LOGI(TAG, "portao-firmware starting, heap=%lu",
             (unsigned long)esp_get_free_heap_size());

    ac_init(&s_access, MAX_FAILED_ATTEMPTS, LOCKOUT_SECONDS);
    ESP_LOGI(TAG, "access_core ready, %u slots, clock untrusted (fail closed)",
             (unsigned)AC_MAX_SLOTS);

    /* Milestone 3: time_sync_start()     - NTP + DS3231, sets clock_trusted
     * Milestone 4: store_load(&s_access) - restore slots from encrypted NVS
     * Milestone 2: keypad_start()        - feeds strings to the pipeline
     * Milestone 7: telegram_start()      - long-poll, HMAC verify, upsert
     * Milestone 5: relay_init()          - pulse output on AC_GRANTED
     */
}
