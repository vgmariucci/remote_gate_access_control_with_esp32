/*
 * gate_guard - refuse to run a door with bench secrets in it.
 *
 * The rules live in cred_check, pure and tested. This is the part that
 * decides what to do about a failure, and the answer depends on the
 * build:
 *
 *   development  complain loudly on every boot and carry on, because a
 *                bench needs to run with placeholder values
 *   production   refuse to start, because a gate with the example
 *                password is not a gate
 *
 * A device that refuses to boot is visible on the bench. One that
 * boots with admin123 is visible to whoever tries it first.
 */
#include "gate_guard.h"

#include "admin_credentials.h"
#include "cred_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "guard";

static bool report(const char *what, cred_result_t r)
{
    if (r == CRED_OK) {
        return true;
    }
    ESP_LOGE(TAG, "  %-18s %s", what, cred_result_text(r));
    return false;
}

bool gate_guard_check(void)
{
    bool ok = true;

    ok &= report("admin password", cred_check_admin_password(ADMIN_PASSWORD));
    ok &= report("AP passphrase", cred_check_ap_password(PROV_AP_PASSWORD));
    ok &= report("delivery key", cred_check_delivery_key(SYNC_DELIVERY_KEY));
    ok &= report("device salt", cred_check_device_salt(GATE_DEVICE_SALT));
    ok &= report("identity", cred_check_identity(SYNC_GATE_ID, SYNC_URL));

    if (ok) {
        ESP_LOGI(TAG, "credentials look fit for a door");
        return true;
    }

#if CONFIG_GATE_DEV_CONSOLE
    ESP_LOGW(TAG, "**************************************************************");
    ESP_LOGW(TAG, "* The credentials above are not fit for a deployed gate.      *");
    ESP_LOGW(TAG, "* A development build runs anyway. A production build will    *");
    ESP_LOGW(TAG, "* refuse to start until they are fixed.                       *");
    ESP_LOGW(TAG, "**************************************************************");
    return false;
#else
    /* Fail closed, as everything else in this firmware does. Somebody
     * has built for production without editing admin_credentials.h,
     * and the only safe place to discover that is here rather than at
     * the door. */
    ESP_LOGE(TAG, "**************************************************************");
    ESP_LOGE(TAG, "* REFUSING TO START: the credentials above are not fit for a  *");
    ESP_LOGE(TAG, "* deployed gate. Edit main/admin_credentials.h and reflash.   *");
    ESP_LOGE(TAG, "**************************************************************");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGE(TAG, "halted: credentials unfit for deployment");
    }
#endif
}
