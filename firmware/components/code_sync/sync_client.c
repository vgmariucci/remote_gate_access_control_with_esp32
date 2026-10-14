#include "sync_client.h"

#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sync_parse.h"

static const char *TAG = "sync";

#define NVS_NS "gate_sync"
#define NVS_KEY_GEN "gen"
#define HTTP_TIMEOUT 15000

/* How often the task looks at the clock. The period it is deciding
 * about is fifteen minutes, so a second of granularity costs nothing
 * and keeps the task asleep the rest of the time. */
#define TASK_TICK_MS 1000

/* Below the keypad (6) and the portal's DNS (5): neither must ever wait
 * on a network fetch. Above the main loop (1) so a code that has
 * arrived is installed without waiting for the next iteration. */
#define TASK_PRIORITY 4

/* mbedtls with the certificate bundle does not fit in the 3072 the
 * other tasks use. The first successful poll logs what was actually
 * left, so this can be tuned against a measurement. */
#define TASK_STACK 8192

static sync_client_config_t s_cfg;
static uint8_t s_key[32];
static bool s_key_ok;
static uint64_t s_generation;

static sync_ctx_t *s_ctx;
static ac_ctx_t *s_table;
static TaskHandle_t s_task;
static bool s_stack_reported;

/* The whole body, collected by the event handler. Static rather than
 * heap: the size is bounded by the format, and a gate that cannot
 * allocate at the wrong moment should still be able to sync. Only the
 * sync task touches it. */
static char s_body[SYNC_BODY_MAX + 1];
static size_t s_body_len;

static void lock(void)
{
    if (s_cfg.lock != NULL) {
        s_cfg.lock();
    }
}

static void unlock(void)
{
    if (s_cfg.unlock != NULL) {
        s_cfg.unlock();
    }
}

static uint32_t now_ms(void)
{
    return (s_cfg.now_ms != NULL) ? s_cfg.now_ms() : 0;
}

static bool hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
    for (size_t i = 0; i < out_len; i++) {
        char hi = hex[i * 2];
        char lo = hex[i * 2 + 1];
        int h = (hi >= '0' && hi <= '9')   ? hi - '0'
                : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10
                : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10
                                           : -1;
        int l = (lo >= '0' && lo <= '9')   ? lo - '0'
                : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10
                : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10
                                           : -1;
        if (h < 0 || l < 0) {
            return false;
        }
        out[i] = (uint8_t)((h << 4) | l);
    }
    return true;
}

/* The MAC that sync_parse asks for. Kept here so the parser stays pure
 * and host-testable. */
static bool compute_mac(const char *region, size_t len, char out_hex[SYNC_MAC_HEX_LEN + 1],
                        void *user)
{
    (void)user;
    if (!s_key_ok) {
        return false;
    }
    uint8_t digest[32];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md == NULL) {
        return false;
    }
    if (mbedtls_md_hmac(md, s_key, sizeof(s_key), (const unsigned char *)region, len,
                        digest) != 0) {
        return false;
    }
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[i * 2] = hex[digest[i] >> 4];
        out_hex[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    out_hex[SYNC_MAC_HEX_LEN] = '\0';
    return true;
}

static void save_generation(uint64_t gen)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "could not persist the generation; a reboot may re-apply a set");
        return;
    }
    nvs_set_u64(h, NVS_KEY_GEN, gen);
    nvs_commit(h);
    nvs_close(h);
}

esp_err_t sync_client_init(const sync_client_config_t *cfg, sync_ctx_t *ctx)
{
    if (cfg == NULL || cfg->url == NULL || cfg->gate_id == NULL || cfg->delivery_key == NULL ||
        ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* A missing lock would mean the table is edited from the sync task
     * while the main loop is reading it. Refuse rather than race. */
    if (cfg->lock == NULL || cfg->unlock == NULL || cfg->online == NULL ||
        cfg->now_ms == NULL) {
        ESP_LOGE(TAG, "sync needs lock, unlock, online and now_ms; sync disabled");
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *cfg;

    if (strlen(cfg->delivery_key) != 64 || !hex_to_bytes(cfg->delivery_key, s_key, 32)) {
        ESP_LOGE(TAG, "delivery key must be 64 hex characters; sync disabled");
        s_key_ok = false;
        return ESP_ERR_INVALID_ARG;
    }
    s_key_ok = true;

    /* The generation must survive a reboot, or a captured older set
     * could be applied again after a power cut (ADR 0007). */
    uint64_t stored = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u64(h, NVS_KEY_GEN, &stored);
        nvs_close(h);
    }
    s_generation = stored;
    sync_init(ctx, stored, 0);

    ESP_LOGI(TAG, "sync from %s as \"%s\", holding generation %llu", s_cfg.url, s_cfg.gate_id,
             (unsigned long long)stored);
    return ESP_OK;
}

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        /* Refuse rather than truncate: a body cut to fit would fail the
         * signature anyway, but silently keeping the first 2 KB of
         * something larger is the kind of thing that confuses a
         * diagnosis later. */
        if (s_body_len + evt->data_len > SYNC_BODY_MAX) {
            ESP_LOGW(TAG, "response larger than %d bytes; refused", SYNC_BODY_MAX);
            s_body_len = SYNC_BODY_MAX + 1; /* poison it */
            return ESP_OK;
        }
        memcpy(s_body + s_body_len, evt->data, (size_t)evt->data_len);
        s_body_len += (size_t)evt->data_len;
    }
    return ESP_OK;
}

/* Runs with no lock held. This is the part that can take fifteen
 * seconds, and nothing else in the firmware waits for it. */
static esp_err_t fetch(void)
{
    s_body_len = 0;
    memset(s_body, 0, sizeof(s_body));

    esp_http_client_config_t http = {
        .url = s_cfg.url,
        .event_handler = on_http_event,
        .timeout_ms = HTTP_TIMEOUT,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
        .disable_auto_redirect = false,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    int64_t advertised = esp_http_client_get_content_length(client);
    esp_http_client_cleanup(client);

    /* Logged every time: "body cut short" is impossible to diagnose
     * without knowing whether anything arrived at all. Lengths only -
     * the body carries code hashes and is never printed. */
    ESP_LOGI(TAG, "HTTP %d, content-length %lld, received %u byte(s)", status,
             (long long)advertised, (unsigned)s_body_len);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "fetch failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "fetch returned HTTP %d", status);
        return ESP_FAIL;
    }
    if (s_body_len == 0) {
        ESP_LOGW(TAG, "no body received");
        return ESP_FAIL;
    }
    if (s_body_len > SYNC_BODY_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    s_body[s_body_len] = '\0';
    return ESP_OK;
}

static void note_failure(sync_ctx_t *ctx, uint32_t at_ms)
{
    lock();
    sync_failed(ctx, at_ms);
    unlock();
}

/* Does the work once the caller has decided a poll should happen.
 *
 * The lock is taken three times, each around a few microseconds of
 * bookkeeping, and never around fetch(). Widening it to cover the whole
 * function would restore exactly the stall this task exists to remove. */
static bool do_sync(sync_ctx_t *ctx, ac_ctx_t *table, uint32_t at_ms)
{
    if (!s_key_ok) {
        return false;
    }
    if (fetch() != ESP_OK) {
        note_failure(ctx, at_ms);
        return false;
    }

    sync_set_t set;
    sync_parse_result_t pr = sync_parse(s_body, s_cfg.gate_id, compute_mac, NULL, &set);
    if (pr != SYNC_PARSE_OK) {
        /* Never log the body: it carries the code hashes. */
        ESP_LOGW(TAG, "response rejected: %s", sync_parse_text(pr));
        note_failure(ctx, at_ms);
        return false;
    }

    lock();
    sync_result_t ar = sync_apply(ctx, table, &set, at_ms);
    uint64_t gen = ctx->generation;
    size_t held = ac_count(table);
    unlock();

    memset(&set, 0, sizeof(set));

    switch (ar) {
    case SYNC_APPLIED:
    case SYNC_OVERFLOWED:
        ESP_LOGI(TAG, "generation %llu applied: %u code(s)%s", (unsigned long long)gen,
                 (unsigned)held, ar == SYNC_OVERFLOWED ? " (more were sent than fit)" : "");
        if (held == 0) {
            /* Loud on purpose: this is either a deliberate mass
             * revocation or a backend bug, and both deserve a line in
             * the log someone will find (ADR 0007). */
            ESP_LOGW(TAG, "the code set is now EMPTY: every guest code has been revoked");
        }
        s_generation = gen;
        save_generation(s_generation);
        return true;

    case SYNC_UNCHANGED:
        ESP_LOGI(TAG, "generation %llu unchanged", (unsigned long long)gen);
        return false;

    case SYNC_STALE:
        ESP_LOGW(TAG, "refused an older generation than the one held (%llu)",
                 (unsigned long long)gen);
        return false;

    case SYNC_BAD_ENTRY:
    default:
        ESP_LOGW(TAG, "set refused; the table is unchanged");
        return false;
    }
}

static bool poll_is_due(sync_ctx_t *ctx, uint32_t at_ms)
{
    lock();
    bool due = sync_due(ctx, at_ms);
    unlock();
    return due;
}

/* Reported once, after the first poll that actually opened a TLS
 * connection - that is the high-water mark, and guessing at TASK_STACK
 * is worse than measuring it. */
static void report_stack_once(void)
{
    if (s_stack_reported) {
        return;
    }
    s_stack_reported = true;
    /* ESP-IDF's FreeRTOS reports this in bytes, unlike vanilla FreeRTOS
     * which reports words. No scaling. */
    ESP_LOGI(TAG, "sync task stack: %u byte(s) unused of %d",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), TASK_STACK);
}

static void sync_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "sync task started; the main loop no longer waits on the network");

    for (;;) {
        uint32_t at_ms = now_ms();
        if (s_cfg.online() && poll_is_due(s_ctx, at_ms)) {
            do_sync(s_ctx, s_table, at_ms);
            report_stack_once();
        }
        vTaskDelay(pdMS_TO_TICKS(TASK_TICK_MS));
    }
}

esp_err_t sync_client_start(sync_ctx_t *ctx, ac_ctx_t *table)
{
    if (ctx == NULL || table == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_key_ok) {
        ESP_LOGW(TAG, "sync not configured; the task will not start");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task != NULL) {
        return ESP_OK;
    }
    s_ctx = ctx;
    s_table = table;

    if (xTaskCreate(sync_task, "sync", TASK_STACK, NULL, TASK_PRIORITY, &s_task) != pdPASS) {
        /* Not fatal. The gate opens for a valid code with no network at
         * all (ADR 0001); what is lost is learning about new ones. */
        ESP_LOGE(TAG, "could not start the sync task; codes will not refresh");
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t sync_client_poll_now(sync_ctx_t *ctx, ac_ctx_t *table)
{
    if (ctx == NULL || table == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Blocks the console task, not the main loop. The console is a
     * person waiting for an answer; the gate carries on. */
    return do_sync(ctx, table, now_ms()) ? ESP_OK : ESP_FAIL;
}

uint64_t sync_client_generation(void)
{
    /* Read without the lock on purpose: taking it here would deadlock
     * any caller that already holds it, and this value only ever
     * reaches a log line or the console. A torn read across the two
     * halves of the 64-bit word would show one wrong number, once, in
     * the microseconds after a sync - and the right one is a command
     * away. */
    return s_generation;
}
