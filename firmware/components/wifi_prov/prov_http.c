#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "prov_ap.h"
#include "wifi_prov.h"

static const char *TAG = "prov.http";

static httpd_handle_t s_server;

/* Part one: one page, no forms. The Wi-Fi form, the scan list and the
 * admin login arrive next; this exists to prove the AP, the DNS
 * responder and the server come up and go down cleanly. */
static const char PAGE[] =
    "<!DOCTYPE html><html lang=\"pt-br\"><head>"
    "<meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Portao</title><style>"
    "body{font-family:system-ui,sans-serif;margin:0;padding:2rem 1.25rem;"
    "background:#101014;color:#e8e8ea;line-height:1.5}"
    "h1{font-size:1.4rem;margin:0 0 .25rem}"
    "p{margin:.5rem 0;color:#a8a8b0}"
    "b{color:#e8e8ea}"
    "</style></head><body>"
    "<h1>Portao</h1>"
    "<p>Modo de configuracao ativo.</p>"
    "<p>Fecha em <b>%u</b> s.</p>"
    "<p>Rede: <b>%s</b></p>"
    "</body></html>";

static esp_err_t page_get(httpd_req_t *req)
{
    char body[sizeof(PAGE) + 64];
    uint32_t left = wifi_prov_seconds_left((uint32_t)(esp_timer_get_time() / 1000));
    int n = snprintf(body, sizeof(body), PAGE, (unsigned)left, wifi_prov_ssid());

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    /* Nothing here may be cached: the countdown is live and the pages
     * that follow will carry credentials. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, n);
}

/* Anything else - including the connectivity-check URLs phones probe -
 * is redirected to the page, which is what raises the "sign in to
 * network" prompt instead of "no internet". */
static esp_err_t catch_all(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" PROV_AP_IP "/");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t prov_http_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        s_server = NULL;
        return err;
    }

    static const httpd_uri_t root = {
        .uri = "/", .method = HTTP_GET, .handler = page_get, .user_ctx = NULL};
    static const httpd_uri_t rest = {
        .uri = "/*", .method = HTTP_GET, .handler = catch_all, .user_ctx = NULL};
    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &rest);

    ESP_LOGI(TAG, "portal at http://%s/", PROV_AP_IP);
    return ESP_OK;
}

void prov_http_stop(void)
{
    if (s_server == NULL) {
        return;
    }
    httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "web server stopped");
}
