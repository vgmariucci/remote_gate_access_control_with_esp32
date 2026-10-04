#include "prov_nvs.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "prov.nvs";

#define NS "gate_wifi"
#define KEY_SSID "ssid"
#define KEY_PSK "psk"

esp_err_t prov_nvs_save(const prov_creds_t *c)
{
    if (c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, KEY_SSID, c->ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, KEY_PSK, c->psk);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    /* The SSID is fine to log; the passphrase never is. */
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved credentials for \"%s\"", c->ssid);
    }
    return err;
}

bool prov_nvs_load(prov_creds_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t n = sizeof(out->ssid);
    bool ok = (nvs_get_str(h, KEY_SSID, out->ssid, &n) == ESP_OK);
    if (ok) {
        n = sizeof(out->psk);
        if (nvs_get_str(h, KEY_PSK, out->psk, &n) != ESP_OK) {
            out->psk[0] = '\0'; /* an open network saves no passphrase */
        }
    }
    nvs_close(h);
    return ok && out->ssid[0] != '\0';
}

esp_err_t prov_nvs_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_all(h);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}
