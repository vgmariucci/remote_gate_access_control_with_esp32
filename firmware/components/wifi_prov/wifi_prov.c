#include "wifi_prov.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "prov_ap.h"
#include "prov_form.h"

static const char *TAG = "prov";

static prov_ctx_t s_ctx;
static gpio_num_t s_button = GPIO_NUM_NC;
static char s_ssid[PROV_AP_SSID_MAX];
static wifi_prov_config_t s_cfg;
static volatile bool s_finish_requested;

esp_err_t wifi_prov_init(const wifi_prov_config_t *cfg)
{
    if (cfg == NULL || cfg->ap_password == NULL || cfg->admin_user == NULL ||
        cfg->admin_password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    prov_init(&s_ctx);
    s_cfg = *cfg;
    s_button = cfg->button_gpio;
    s_ssid[0] = '\0';

    /* esp_wifi_init needs NVS and the event loop, and they must exist
     * before the first session rather than be created under a button
     * press. Both are idempotent, so app_main may also have called
     * them. */
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

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << s_button),
        .mode = GPIO_MODE_INPUT,
        /* Active low, so a disconnected wire reads "not pressed" and a
         * shorted one is visible as a session that will not close. */
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "provisioning button on GPIO %d, hold %u s to open the portal",
             (int)s_button, (unsigned)(PROV_HOLD_MS / 1000));
    return ESP_OK;
}

wifi_prov_evt_t wifi_prov_tick(uint32_t now_ms)
{
    if (s_button == GPIO_NUM_NC) {
        return WIFI_PROV_EVT_NONE;
    }

    /* An admin who pressed "finish" is served first: the handler only
     * raised a flag, and this is the task that may safely stop the
     * server. */
    if (s_finish_requested) {
        s_finish_requested = false;
        if (prov_close(&s_ctx) == PROV_ACTION_CLOSE_AP) {
            prov_ap_stop();
            s_ssid[0] = '\0';
            ESP_LOGI(TAG, "provisioning portal closed by the admin");
            return WIFI_PROV_EVT_CLOSED;
        }
    }

    bool down = (gpio_get_level(s_button) == 0);
    prov_action_t a = prov_button(&s_ctx, down, now_ms);
    if (a == PROV_ACTION_NONE) {
        a = prov_tick(&s_ctx, now_ms);
    }

    switch (a) {
    case PROV_ACTION_OPEN_AP:
        if (prov_ap_start(s_cfg.ap_password, s_ssid, sizeof(s_ssid)) != ESP_OK) {
            /* The radio did not come up. Close the session rather than
             * leaving prov_logic believing a portal is running. */
            prov_close(&s_ctx);
            s_ssid[0] = '\0';
            return WIFI_PROV_EVT_NONE;
        }
        ESP_LOGW(TAG, "provisioning portal OPEN for %u s - join %s and browse to http://%s/",
                 (unsigned)(PROV_SESSION_MS / 1000), s_ssid, PROV_AP_IP);
        return WIFI_PROV_EVT_OPENED;

    case PROV_ACTION_CLOSE_AP:
        prov_ap_stop();
        s_ssid[0] = '\0';
        ESP_LOGI(TAG, "provisioning portal closed");
        return WIFI_PROV_EVT_CLOSED;

    case PROV_ACTION_NONE:
    default:
        return WIFI_PROV_EVT_NONE;
    }
}

void wifi_prov_close(void)
{
    if (prov_close(&s_ctx) == PROV_ACTION_CLOSE_AP) {
        prov_ap_stop();
        s_ssid[0] = '\0';
    }
}

bool wifi_prov_is_open(void)
{
    return prov_is_open(&s_ctx);
}

uint32_t wifi_prov_seconds_left(uint32_t now_ms)
{
    return prov_seconds_left(&s_ctx, now_ms);
}

const char *wifi_prov_ssid(void)
{
    return s_ssid;
}

/* ---------------------------------------------------- session bridge
 *
 * The HTTP layer calls these rather than touching the prov_logic
 * context, so the failure counter and the teardown stay in one place.
 */

bool prov_session_login(const char *user, const char *password)
{
    if (user == NULL || password == NULL) {
        return false;
    }
    /* Both comparisons run to completion: a wrong username must not
     * return faster than a wrong password. */
    bool user_ok = prov_ct_str_equal(user, s_cfg.admin_user);
    bool pass_ok = prov_ct_str_equal(password, s_cfg.admin_password);
    return prov_login(&s_ctx, user_ok && pass_ok);
}

bool prov_session_is_open(void)
{
    return prov_is_open(&s_ctx);
}

bool prov_session_is_authenticated(void)
{
    return prov_is_authenticated(&s_ctx);
}

void prov_session_logout(void)
{
    /* Drops authentication without ending the session: the admin can
     * log in again until the five minutes run out. */
    if (prov_is_open(&s_ctx)) {
        s_ctx.authenticated = false;
    }
}

void prov_session_finish(void)
{
    /* Only a request. Tearing the portal down here would call
     * httpd_stop() from inside an HTTP handler, on the very task
     * httpd_stop waits for: a self-join that hangs the server and,
     * when the session later times out, the main loop behind it.
     * The main loop does the work on its next tick instead. */
    s_finish_requested = true;
}
