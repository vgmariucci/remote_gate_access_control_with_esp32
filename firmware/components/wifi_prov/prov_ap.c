#include "prov_ap.h"

#include "esp_log.h"
#include "net_link.h"

static const char *TAG = "prov.ap";

static bool s_running;

/* The radio belongs to net_link now: the station needs it continuously
 * and the portal only borrows an interface. What stays here is the
 * order the three pieces come up and go down in. */
esp_err_t prov_ap_start(const char *password, char *out_ssid, size_t out_len)
{
    if (s_running) {
        return ESP_OK;
    }
    esp_err_t err = net_link_ap_up(password, out_ssid, out_len);
    if (err != ESP_OK) {
        return err;
    }

    if (prov_dns_start() != ESP_OK) {
        ESP_LOGW(TAG, "DNS responder failed: the portal still works at http://%s", PROV_AP_IP);
    }
    if (prov_http_start() != ESP_OK) {
        ESP_LOGE(TAG, "web server failed to start");
        prov_dns_stop();
        net_link_ap_down();
        return ESP_FAIL;
    }
    s_running = true;
    return ESP_OK;
}

void prov_ap_stop(void)
{
    if (!s_running) {
        return;
    }
    /* Stop accepting requests, then the name service, then the radio
     * interface. */
    prov_http_stop();
    prov_dns_stop();
    net_link_ap_down();
    s_running = false;
}
