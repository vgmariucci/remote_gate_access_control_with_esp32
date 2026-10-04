#include "net_link.h"

#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "net_retry.h"
#include "nvs_flash.h"
#include "prov_nvs.h"

static const char *TAG = "net";

#define AP_CHANNEL 1
#define AP_MAX_CLIENTS 1
#define BIT_GOT_IP BIT0
#define BIT_STA_FAIL BIT1

static esp_netif_t *s_netif_sta;
static esp_netif_t *s_netif_ap;
static EventGroupHandle_t s_events;
static net_link_time_cb s_on_time;

static bool s_wifi_inited;  /* esp_wifi_init done; never undone */
static bool s_wifi_started; /* esp_wifi_start done */
static bool s_want_sta;     /* the station interface exists */
/* ... and we actually want it associated. Raising the interface for a
 * scan is not a reason to join anything: the driver refuses to scan
 * while a connect is in flight, so an automatic connect on STA_START
 * makes every scan fail. */
static bool s_want_join;
static bool s_want_ap;
static bool s_sta_connected;
static bool s_time_synced;
static char s_ip[16] = "-";

static net_retry_t s_retry;

/* ------------------------------------------------------------ modes */

/* One place decides the radio mode, from what is wanted rather than
 * from who called last. Two callers each setting the mode directly is
 * how an AP disappears the moment the station reconnects. */
/* esp_wifi wants mode, then config, then start - in that order. Set
 * the mode here and start separately, so a caller can configure the
 * interface in between. Starting an AP whose SSID has not been set
 * yet fails quietly and leaves a radio that logs "up" and beacons
 * nothing. */
static esp_err_t apply_mode(void)
{
    wifi_mode_t want;
    if (s_want_ap && s_want_sta) {
        want = WIFI_MODE_APSTA;
    } else if (s_want_ap) {
        want = WIFI_MODE_AP;
    } else if (s_want_sta) {
        want = WIFI_MODE_STA;
    } else {
        ESP_LOGI(TAG, "radio off");
        esp_err_t err = esp_wifi_stop();
        if (err == ESP_OK) {
            s_wifi_started = false;
        }
        return err;
    }

    esp_err_t err = esp_wifi_set_mode(want);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_mode(%d) failed: %s", (int)want, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t ensure_started(void)
{
    if (s_wifi_started) {
        return ESP_OK;
    }
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }
    s_wifi_started = true;
    return ESP_OK;
}

/* ------------------------------------------------------------ events */

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    switch (id) {
    case WIFI_EVENT_STA_START:
        if (s_want_join) {
            esp_wifi_connect();
        }
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        bool was_connected = s_sta_connected;
        s_sta_connected = false;
        snprintf(s_ip, sizeof(s_ip), "-");
        xEventGroupSetBits(s_events, BIT_STA_FAIL);

        if (!s_want_join) {
            break; /* we asked for this */
        }
        if (was_connected) {
            /* A link that was up and dropped: the router rebooted, or
             * we moved out of range. Retry at once rather than
             * inheriting an old backoff. */
            ESP_LOGW(TAG, "station disconnected");
            net_retry_disconnected(&s_retry, (uint32_t)(esp_timer_get_time() / 1000));
        } else {
            net_retry_failed(&s_retry, (uint32_t)(esp_timer_get_time() / 1000));
            if (net_retry_should_log(&s_retry)) {
                ESP_LOGW(TAG, "station connect failed; next try in %u ms",
                         (unsigned)net_retry_delay_ms(&s_retry));
            }
        }
        break;
    }

    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "AP client " MACSTR " joined", MAC2STR(e->mac));
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "AP client " MACSTR " left", MAC2STR(e->mac));
        break;
    }
    default:
        break;
    }
}

static void start_sntp(void)
{
    if (s_time_synced) {
        return;
    }
    /* One shot per boot. The DS3231 keeps time well enough that the
     * gate does not need a running SNTP client, and the RTC is the
     * authority once set (ADR 0002). */
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.start = true;
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed");
        return;
    }
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) == ESP_OK) {
        time_t now = 0;
        time(&now);
        s_time_synced = true;
        ESP_LOGI(TAG, "time from the network: %lld", (long long)now);
        if (s_on_time != NULL) {
            s_on_time((int64_t)now);
        }
    } else {
        ESP_LOGW(TAG, "no SNTP reply; the RTC keeps the time");
    }
    esp_netif_sntp_deinit();
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
    s_sta_connected = true;
    net_retry_connected(&s_retry);
    xEventGroupSetBits(s_events, BIT_GOT_IP);
    ESP_LOGI(TAG, "station up at %s", s_ip);

    start_sntp();
}

/* ------------------------------------------------------------- init */

esp_err_t net_link_init(net_link_time_cb on_time)
{
    s_on_time = on_time;
    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    net_retry_init(&s_retry, 0);

    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    if (nvs != ESP_OK) {
        return nvs;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t loop = esp_event_loop_create_default();
    if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) {
        return loop;
    }
    return ESP_OK;
}

/* esp_wifi_init is done once and never undone: tearing the driver down
 * and bringing it back was a source of failure modes for no gain, and
 * the memory is committed the moment any network is wanted at all. */
static esp_err_t ensure_wifi(void)
{
    if (s_wifi_inited) {
        return ESP_OK;
    }
    s_netif_sta = esp_netif_create_default_wifi_sta();
    s_netif_ap = esp_netif_create_default_wifi_ap();
    if (s_netif_sta == NULL || s_netif_ap == NULL) {
        return ESP_FAIL;
    }

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        return err;
    }
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi,
                                                        NULL, NULL));
    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL, NULL));

    /* Credentials live in our own NVS namespace, not the driver's. */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    s_wifi_inited = true;
    return ESP_OK;
}

/* -------------------------------------------------------- station */

static esp_err_t sta_configure(const char *ssid, const char *psk)
{
    wifi_config_t cfg = {0};
    snprintf((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), "%s", ssid);
    snprintf((char *)cfg.sta.password, sizeof(cfg.sta.password), "%s", psk ? psk : "");
    /* An open network is accepted, but WEP is not: prov_logic refuses
     * it at the form, and this makes the refusal structural. */
    cfg.sta.threshold.authmode =
        (psk != NULL && psk[0] != '\0') ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    return esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

esp_err_t net_link_sta_start(void)
{
    prov_creds_t creds;
    if (!prov_nvs_load(&creds)) {
        ESP_LOGI(TAG, "no WiFi credentials stored; the gate runs offline");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = ensure_wifi();
    if (err != ESP_OK) {
        return err;
    }
    bool was_started = s_wifi_started;
    s_want_sta = true;
    s_want_join = true;
    err = apply_mode();
    if (err == ESP_OK) {
        err = sta_configure(creds.ssid, creds.psk);
    }
    if (err == ESP_OK) {
        err = ensure_started();
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "joining \"%s\"", creds.ssid);
        /* WIFI_EVENT_STA_START calls connect for us when the driver
         * was not already running. Calling it here as well makes the
         * driver log an error at the loser of that race, so leave it
         * to the event unless the driver is up already. */
        if (was_started) {
            esp_wifi_connect();
        }
    }
    memset(&creds, 0, sizeof(creds));
    return err;
}

void net_link_sta_stop(void)
{
    if (!s_want_sta) {
        return;
    }
    s_want_sta = false;
    s_want_join = false;
    s_sta_connected = false;
    esp_wifi_disconnect();
    apply_mode();
}

void net_link_tick(uint32_t now_ms)
{
    if (!s_want_join || s_sta_connected) {
        return;
    }
    if (net_retry_due(&s_retry, now_ms)) {
        /* Pushed forward before the attempt, so a connect that hangs
         * does not produce a retry storm when it finally fails. */
        net_retry_failed(&s_retry, now_ms);
        esp_wifi_connect();
    }
}

bool net_link_sta_connected(void)
{
    return s_sta_connected;
}

const char *net_link_sta_ip(void)
{
    return s_ip;
}

/* ------------------------------------------------------------- AP */

esp_err_t net_link_ap_up(const char *password, char *out_ssid, size_t out_len)
{
    if (password == NULL || strlen(password) < 8 || strlen(password) > 63) {
        ESP_LOGE(TAG, "AP passphrase must be 8-63 characters; refusing to start");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_wifi();
    if (err != ESP_OK) {
        return err;
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    wifi_config_t cfg = {0};
    int n = snprintf((char *)cfg.ap.ssid, sizeof(cfg.ap.ssid), "PORTAO-%02X%02X%02X", mac[3],
                     mac[4], mac[5]);
    cfg.ap.ssid_len = (uint8_t)n;
    cfg.ap.channel = AP_CHANNEL;
    cfg.ap.max_connection = AP_MAX_CLIENTS;
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    snprintf((char *)cfg.ap.password, sizeof(cfg.ap.password), "%s", password);

    s_want_ap = true;
    err = apply_mode();
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "AP set_config failed: %s", esp_err_to_name(err));
        }
    }
    if (err == ESP_OK) {
        err = ensure_started();
    }
    if (err != ESP_OK) {
        s_want_ap = false;
        apply_mode();
        return err;
    }
    if (out_ssid != NULL && out_len > 0) {
        snprintf(out_ssid, out_len, "%s", (const char *)cfg.ap.ssid);
    }
    ESP_LOGI(TAG, "AP up: %s on channel %d, WPA2", (const char *)cfg.ap.ssid, AP_CHANNEL);
    return ESP_OK;
}

void net_link_ap_down(void)
{
    if (!s_want_ap) {
        return;
    }
    s_want_ap = false;
    apply_mode();
    ESP_LOGI(TAG, "AP down");
}

/* ----------------------------------------------------- scan, verify */

esp_err_t net_link_scan(wifi_ap_record_t *out, uint16_t *count)
{
    if (out == NULL || count == NULL || *count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_wifi();
    if (err != ESP_OK) {
        return err;
    }

    /* Scanning needs a station interface. If the station is not wanted,
     * raise it for the scan and put the mode back afterwards. */
    bool added_sta = !s_want_sta;
    if (added_sta) {
        s_want_sta = true;
        apply_mode();
        ensure_started();
    }

    wifi_scan_config_t scan = {.show_hidden = false};
    err = esp_wifi_scan_start(&scan, true);
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_records(count, out);
    }
    if (err != ESP_OK) {
        /* Silence here reads as "no networks nearby", which sends
         * someone hunting for an antenna fault that is not there. */
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        *count = 0;
    }

    if (added_sta) {
        s_want_sta = false;
        apply_mode();
    }
    return err;
}

esp_err_t net_link_try_credentials(const char *ssid, const char *psk, uint32_t timeout_ms)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_wifi();
    if (err != ESP_OK) {
        return err;
    }

    bool had_sta = s_want_sta;
    bool had_join = s_want_join;
    prov_creds_t previous;
    bool had_previous = prov_nvs_load(&previous);

    s_want_sta = true;
    s_want_join = true;
    apply_mode();
    ensure_started();
    esp_wifi_disconnect();
    xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_STA_FAIL);

    err = sta_configure(ssid, psk);
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }

    EventBits_t bits = 0;
    if (err == ESP_OK) {
        bits = xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_STA_FAIL, pdTRUE, pdFALSE,
                                   pdMS_TO_TICKS(timeout_ms));
    }
    bool ok = (bits & BIT_GOT_IP) != 0;
    ESP_LOGI(TAG, "credential test for \"%s\": %s", ssid, ok ? "connected" : "failed");

    /* Put the station back the way it was, whatever happened: a failed
     * test must not cost the gate the connection it already had. */
    esp_wifi_disconnect();
    if (had_previous) {
        sta_configure(previous.ssid, previous.psk);
    }
    s_want_sta = had_sta;
    s_want_join = had_join;
    apply_mode();
    if (had_join) {
        esp_wifi_connect();
    }
    memset(&previous, 0, sizeof(previous));

    return ok ? ESP_OK : ESP_FAIL;
}