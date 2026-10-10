#include "sync_client.h"

#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/md.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sync_parse.h"

static const char *TAG = "sync";

#define NVS_NS "gate_sync"
#define NVS_KEY_GEN "gen"
#define HTTP_TIMEOUT 15000

static sync_client_config_t s_cfg;
static uint8_t s_key[32];
static bool s_key_ok;
static uint64_t s_generation;

/* The whole body, collected by the event handler. Static rather than
 * heap: the size is bounded by the format, and a gate that cannot
 * allocate at the wrong moment should still be able to sync. */
static char s_body[SYNC_BODY_MAX + 1];
static size_t s_body_len;

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

/* Does the work once the caller has decided a poll should happen. */
static bool do_sync(sync_ctx_t *ctx, ac_ctx_t *table, uint32_t now_ms)
{
    if (!s_key_ok) {
        return false;
    }
    if (fetch() != ESP_OK) {
        sync_failed(ctx, now_ms);
        return false;
    }

    sync_set_t set;
    sync_parse_result_t pr = sync_parse(s_body, s_cfg.gate_id, compute_mac, NULL, &set);
    if (pr != SYNC_PARSE_OK) {
        /* Never log the body: it carries the code hashes. */
        ESP_LOGW(TAG, "response rejected: %s", sync_parse_text(pr));
        sync_failed(ctx, now_ms);
        return false;
    }

    sync_result_t ar = sync_apply(ctx, table, &set, now_ms);
    memset(&set, 0, sizeof(set));

    switch (ar) {
    case SYNC_APPLIED:
    case SYNC_OVERFLOWED:
        ESP_LOGI(TAG, "generation %llu applied: %u code(s)%s",
                 (unsigned long long)ctx->generation, (unsigned)ac_count(table),
                 ar == SYNC_OVERFLOWED ? " (more were sent than fit)" : "");
        if (ac_count(table) == 0) {
            /* Loud on purpose: this is either a deliberate mass
             * revocation or a backend bug, and both deserve a line in
             * the log someone will find (ADR 0007). */
            ESP_LOGW(TAG, "the code set is now EMPTY: every guest code has been revoked");
        }
        s_generation = ctx->generation;
        save_generation(s_generation);
        return true;

    case SYNC_UNCHANGED:
        ESP_LOGI(TAG, "generation %llu unchanged", (unsigned long long)ctx->generation);
        return false;

    case SYNC_STALE:
        ESP_LOGW(TAG, "refused an older generation than the one held (%llu)",
                 (unsigned long long)ctx->generation);
        return false;

    case SYNC_BAD_ENTRY:
    default:
        ESP_LOGW(TAG, "set refused; the table is unchanged");
        return false;
    }
}

bool sync_client_tick(sync_ctx_t *ctx, ac_ctx_t *table, uint32_t now_ms, bool online)
{
    if (ctx == NULL || table == NULL || !online || !sync_due(ctx, now_ms)) {
        return false;
    }
    return do_sync(ctx, table, now_ms);
}

esp_err_t sync_client_poll_now(sync_ctx_t *ctx, ac_ctx_t *table, uint32_t now_ms)
{
    if (ctx == NULL || table == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return do_sync(ctx, table, now_ms) ? ESP_OK : ESP_FAIL;
}

uint64_t sync_client_generation(void)
{
    return s_generation;
}