#include "prov_ap.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "prov.ap";

#define PROV_AP_CHANNEL 1
#define PROV_AP_MAX_CLIENTS 1 /* one admin; a second client is not a use case */
#define PROV_AP_BEACON_MS 100

static esp_netif_t *s_netif;
static bool s_running;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "client " MACSTR " joined", MAC2STR(e->mac));
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "client " MACSTR " left", MAC2STR(e->mac));
    }
}

esp_err_t prov_ap_start(const char *password, char *out_ssid, size_t out_len)
{
    if (s_running) {
        return ESP_OK;
    }
    /* The AP passphrase is the outer gate: without it, radio range
     * alone reaches the login page (ADR 0006). Refuse to broadcast
     * rather than fall back to an open network. */
    if (password == NULL || strlen(password) < 8 || strlen(password) > 63) {
        ESP_LOGE(TAG, "AP passphrase must be 8-63 characters; refusing to start");
        return ESP_ERR_INVALID_ARG;
    }

    s_netif = esp_netif_create_default_wifi_ap();
    if (s_netif == NULL) {
        return ESP_FAIL;
    }

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        goto fail_netif;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event,
                                              NULL, NULL);
    if (err != ESP_OK) {
        goto fail_wifi;
    }

    /* Last three MAC bytes, so an installer with two gates can tell
     * which one they are joining. */
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_AP, mac);

    wifi_config_t cfg = {0};
    int n = snprintf((char *)cfg.ap.ssid, sizeof(cfg.ap.ssid), "PORTAO-%02X%02X%02X", mac[3],
                     mac[4], mac[5]);
    cfg.ap.ssid_len = (uint8_t)n;
    cfg.ap.channel = PROV_AP_CHANNEL;
    cfg.ap.max_connection = PROV_AP_MAX_CLIENTS;
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.ap.beacon_interval = PROV_AP_BEACON_MS;
    cfg.ap.pmf_cfg.required = false;
    snprintf((char *)cfg.ap.password, sizeof(cfg.ap.password), "%s", password);

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        goto fail_wifi;
    }

    if (out_ssid != NULL && out_len > 0) {
        snprintf(out_ssid, out_len, "%s", (const char *)cfg.ap.ssid);
    }
    ESP_LOGI(TAG, "AP up: %s on channel %d, WPA2", (const char *)cfg.ap.ssid, PROV_AP_CHANNEL);

    if (prov_dns_start() != ESP_OK) {
        ESP_LOGW(TAG, "DNS responder failed: the portal still works at http://%s", PROV_AP_IP);
    }
    if (prov_http_start() != ESP_OK) {
        ESP_LOGE(TAG, "web server failed to start");
        goto fail_started;
    }

    s_running = true;
    return ESP_OK;

fail_started:
    prov_dns_stop();
    esp_wifi_stop();
fail_wifi:
    esp_wifi_deinit();
fail_netif:
    esp_netif_destroy_default_wifi(s_netif);
    s_netif = NULL;
    ESP_LOGE(TAG, "could not start the portal");
    return err == ESP_OK ? ESP_FAIL : err;
}

void prov_ap_stop(void)
{
    if (!s_running) {
        return;
    }
    /* Order matters: stop accepting requests, then the name service,
     * then the radio. */
    prov_http_stop();
    prov_dns_stop();

    esp_wifi_stop();
    esp_wifi_deinit();
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event);
    if (s_netif != NULL) {
        esp_netif_destroy_default_wifi(s_netif);
        s_netif = NULL;
    }
    s_running = false;
    ESP_LOGI(TAG, "AP down; the radio is off");
}
