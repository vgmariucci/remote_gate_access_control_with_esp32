#include "tg_client.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "cred_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "tg_poll.h"
#include "tg_proto.h"

static const char *TAG = "tg";

#define NVS_NS "gate_tg"
#define NVS_KEY_OFFSET "offset"   /* Telegram's receipt */
#define NVS_KEY_COUNTER "counter" /* tg_proto's replay boundary */

/* The HTTP timeout must exceed the long poll, or the client gives up on
 * a request that is working exactly as intended. */
#define HTTP_TIMEOUT_MS ((TG_LONGPOLL_S + 15) * 1000)

/* At most this many updates per request. Bounds the response so a
 * static buffer is enough, and bounds how long one batch holds the
 * task before it looks at the clock again. */
#define BATCH_LIMIT 5

/* Five updates of envelope plus a 256-byte line each. Refused rather
 * than truncated: a body cut to fit would fail to parse anyway, and
 * silently keeping the first part of something larger is what makes a
 * diagnosis impossible later. */
#define BODY_MAX 6144

/* Below sync_client (4): when both want the lock, the channel that is
 * the correctness mechanism goes first. Still above the main loop. */
#define TASK_PRIORITY 3
#define TASK_STACK 8192

static tg_client_config_t s_cfg;
static uint8_t s_key[32];
static bool s_enabled;
static const char *s_state = "not configured";

static tg_poll_ctx_t s_poll;
static ac_ctx_t *s_table;
static TaskHandle_t s_task;
static bool s_stack_reported;

static uint64_t s_counter; /* tg_proto's, persisted separately */

static char s_body[BODY_MAX + 1];
static size_t s_body_len;
static char s_url[320];

/* ------------------------------------------------------------ small */

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

/* The MAC tg_parse asks for, so the parser stays free of any crypto
 * library and host-testable. Same construction and same key as
 * sync_client's: one delivery key, two transports. */
static bool compute_mac(const char *region, size_t len, char out_hex[TG_MAC_HEX_LEN + 1],
                        void *user)
{
    (void)user;
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
    out_hex[TG_MAC_HEX_LEN] = '\0';
    return true;
}

/* --------------------------------------------------------- the NVS */

static void save_u64(const char *key, uint64_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "could not persist %s", key);
        return;
    }
    nvs_set_u64(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

static uint64_t load_u64(const char *key)
{
    uint64_t v = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u64(h, key, &v);
        nvs_close(h);
    }
    return v;
}

/* ------------------------------------------------------------- HTTP */

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (s_body_len + (size_t)evt->data_len > BODY_MAX) {
            s_body_len = BODY_MAX + 1; /* poison it */
            return ESP_OK;
        }
        memcpy(s_body + s_body_len, evt->data, (size_t)evt->data_len);
        s_body_len += (size_t)evt->data_len;
    }
    return ESP_OK;
}

/* Runs with no lock held. This is the call that blocks for most of a
 * minute, and nothing else in the firmware waits for it. */
static esp_err_t fetch_updates(uint64_t offset, int *out_status)
{
    s_body_len = 0;
    s_body[0] = '\0';
    *out_status = 0;

    int n = snprintf(s_url, sizeof(s_url),
                     "https://api.telegram.org/bot%s/getUpdates"
                     "?offset=%llu&timeout=%d&limit=%d",
                     s_cfg.bot_token, (unsigned long long)offset, TG_LONGPOLL_S, BATCH_LIMIT);
    if (n <= 0 || (size_t)n >= sizeof(s_url)) {
        /* The token is the only variable-length part, and cred_check
         * has already bounded it, so this is a programming error
         * rather than a configuration one. */
        ESP_LOGE(TAG, "request URL does not fit");
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_config_t http = {
        .url = s_url,
        .event_handler = on_http_event,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    *out_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGD(TAG, "poll failed: %s", esp_err_to_name(err));
        return err;
    }
    if (s_body_len > BODY_MAX) {
        ESP_LOGW(TAG, "response larger than %d bytes; refused", BODY_MAX);
        return ESP_ERR_INVALID_SIZE;
    }
    s_body[s_body_len] = '\0';
    return ESP_OK;
}

/* ------------------------------------------------------- one update */

/* Installs an authenticated message. The lock is held for the table
 * call and nothing else. */
static void apply(const tg_msg_t *m)
{
    lock();
    if (m->op == TG_OP_ADD) {
        int slot = ac_upsert(s_table, m->id, m->hash, m->valid_from, m->valid_until);
        unlock();
        if (slot < 0) {
            ESP_LOGW(TAG, "add %s refused: the table is full", m->id);
        } else {
            ESP_LOGI(TAG, "add %s applied to slot %d", m->id, slot);
        }
    } else {
        bool removed = ac_revoke(s_table, m->id);
        unlock();
        ESP_LOGI(TAG, "revoke %s: %s", m->id, removed ? "removed" : "was not held");
    }
}

/* Returns the number of updates consumed. */
static int handle_body(void)
{
    cJSON *root = cJSON_Parse(s_body);
    if (root == NULL) {
        ESP_LOGW(TAG, "response was not JSON");
        return -1;
    }

    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    if (!cJSON_IsTrue(ok)) {
        /* Telegram says no. The description would help, but it can
         * echo the request, so only the fact is logged. */
        ESP_LOGW(TAG, "Telegram refused the request");
        cJSON_Delete(root);
        return -1;
    }

    const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (!cJSON_IsArray(result)) {
        cJSON_Delete(root);
        return 0;
    }

    int consumed = 0;
    int refused = 0;
    tg_result_t last_refusal = TG_OK;

    const cJSON *upd = NULL;
    cJSON_ArrayForEach(upd, result)
    {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(upd, "update_id");
        if (!cJSON_IsNumber(id)) {
            continue; /* not an update; nothing to acknowledge */
        }

        /* Consumed whatever happens next. An update that is never
         * acknowledged comes back on every request until it expires,
         * so one forged line would hide every real code behind it for
         * a day. Acknowledging receipt is not accepting content. */
        tg_poll_consume(&s_poll, (uint64_t)cJSON_GetNumberValue(id));
        consumed++;

        /* A channel post carries the same text as a message. Anything
         * else - an edit, a join, a reaction - is consumed and
         * ignored. */
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(upd, "message");
        if (msg == NULL) {
            msg = cJSON_GetObjectItemCaseSensitive(upd, "channel_post");
        }
        if (msg == NULL) {
            continue;
        }
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(msg, "text");
        if (!cJSON_IsString(text) || text->valuestring == NULL) {
            continue;
        }
        if (strlen(text->valuestring) >= TG_LINE_MAX) {
            continue; /* not one of ours; tg_parse would refuse it */
        }

        tg_msg_t m;
        tg_result_t r = tg_parse(text->valuestring, s_counter, compute_mac, NULL, &m);
        if (r != TG_OK) {
            /* Never log the line: it carries a code hash. And never
             * log one line per refusal - anyone who finds the chat can
             * post a thousand, and the log is what you need to
             * diagnose that. */
            refused++;
            last_refusal = r;
            continue;
        }

        apply(&m);

        /* Only now. The counter is the replay boundary and a refused
         * message must never move it. */
        s_counter = m.counter;
        save_u64(NVS_KEY_COUNTER, s_counter);
    }

    if (refused > 0) {
        ESP_LOGW(TAG, "%d message(s) refused, last: %s", refused,
                 tg_result_text(last_refusal));
    }

    cJSON_Delete(root);
    return consumed;
}

/* -------------------------------------------------------- the task */

static void report_stack_once(void)
{
    if (s_stack_reported) {
        return;
    }
    s_stack_reported = true;
    /* ESP-IDF's FreeRTOS reports this in bytes, not words. */
    ESP_LOGI(TAG, "tg task stack: %u byte(s) unused of %d",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), TASK_STACK);
}

static void poll_once(void)
{
    int status = 0;
    uint64_t offset = tg_poll_next_offset(&s_poll);

    esp_err_t err = fetch_updates(offset, &status);
    report_stack_once();

    if (status == 401 || status == 404) {
        /* The token is wrong, revoked, or the bot is gone. Retrying
         * every five minutes forever buys nothing but a few hundred
         * pointless handshakes a day and an invitation to be
         * rate-limited. Loud once, then silence. */
        ESP_LOGE(TAG, "Telegram returned HTTP %d: the bot token is wrong or revoked.", status);
        ESP_LOGE(TAG, "The courier is off until this is fixed and the gate reboots.");
        ESP_LOGE(TAG, "Codes still arrive on the fifteen-minute poll (ADR 0007).");
        tg_poll_fatal(&s_poll);
        s_state = "stopped: the bot token was refused";
        return;
    }

    if (err != ESP_OK || status != 200) {
        tg_poll_failed(&s_poll, now_ms());
        s_state = "retrying";
        return;
    }

    int consumed = handle_body();
    if (consumed < 0) {
        tg_poll_failed(&s_poll, now_ms());
        s_state = "retrying";
        return;
    }

    tg_poll_ok(&s_poll, now_ms());
    s_state = "listening";

    if (tg_poll_should_persist(&s_poll)) {
        save_u64(NVS_KEY_OFFSET, s_poll.offset);
        tg_poll_persisted(&s_poll);
    }
}

static void tg_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "courier started; a new code now arrives in seconds, not minutes");

    for (;;) {
        if (tg_poll_is_fatal(&s_poll)) {
            /* Nothing will change until someone reconfigures and
             * reboots. Sleep long rather than spin. */
            vTaskDelay(pdMS_TO_TICKS(60000));
            continue;
        }
        if (tg_poll_due(&s_poll, now_ms(), s_cfg.online())) {
            poll_once();
        }
        /* Unconditional, and outside the branch on purpose: tg_poll
         * puts a floor between requests, so a task that looped without
         * delaying would spin through that gap at a priority above the
         * main loop. */
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

/* --------------------------------------------------------- the API */

esp_err_t tg_client_init(const tg_client_config_t *cfg)
{
    if (cfg == NULL || cfg->bot_token == NULL || cfg->delivery_key == NULL ||
        cfg->lock == NULL || cfg->unlock == NULL || cfg->online == NULL ||
        cfg->now_ms == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *cfg;
    s_enabled = false;

    cred_result_t cr = cred_check_bot_token(cfg->bot_token);
    if (cr != CRED_OK) {
        /* Not fatal, by design. ADR 0007 makes this channel an
         * optimisation, and a door that will not open because a chat
         * bot is misconfigured would be a worse failure than no chat
         * bot. Said plainly so it is not mistaken for a bug later. */
        ESP_LOGW(TAG, "bot token %s: the Telegram courier is off.", cred_result_text(cr));
        ESP_LOGW(TAG, "Codes still arrive on the fifteen-minute poll (ADR 0007).");
        s_state = "off: no usable bot token";
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(cfg->delivery_key) != 64 || !hex_to_bytes(cfg->delivery_key, s_key, 32)) {
        ESP_LOGW(TAG, "delivery key is not 64 hex characters; the courier is off");
        s_state = "off: no usable delivery key";
        return ESP_ERR_INVALID_ARG;
    }

    /* Both numbers survive a reboot, and they are different numbers.
     * The counter is what stops a captured message being replayed; the
     * offset only stops Telegram resending what we already saw. */
    s_counter = load_u64(NVS_KEY_COUNTER);
    tg_poll_init(&s_poll, load_u64(NVS_KEY_OFFSET), cfg->now_ms());

    s_enabled = true;
    s_state = "ready";
    ESP_LOGI(TAG, "courier ready, holding counter %llu, offset %llu",
             (unsigned long long)s_counter, (unsigned long long)s_poll.offset);
    return ESP_OK;
}

esp_err_t tg_client_start(ac_ctx_t *table)
{
    if (table == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_enabled) {
        return ESP_ERR_INVALID_STATE; /* already logged why */
    }
    if (s_task != NULL) {
        return ESP_OK;
    }
    s_table = table;

    if (xTaskCreate(tg_task, "tg", TASK_STACK, NULL, TASK_PRIORITY, &s_task) != pdPASS) {
        ESP_LOGW(TAG, "could not start the courier task; codes arrive on the poll instead");
        s_task = NULL;
        s_state = "off: no task";
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool tg_client_enabled(void)
{
    return s_enabled && !tg_poll_is_fatal(&s_poll);
}

uint64_t tg_client_offset(void)
{
    return s_poll.offset;
}

uint64_t tg_client_counter(void)
{
    return s_counter;
}

const char *tg_client_state_text(void)
{
    return s_state;
}
