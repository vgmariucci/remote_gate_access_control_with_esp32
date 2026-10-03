/*
 * prov_nvs - the Wi-Fi credentials the portal saves.
 *
 * Kept apart from the portal so the station code can read them at boot
 * without pulling in the web server.
 */
#ifndef PROV_NVS_H
#define PROV_NVS_H

#include "esp_err.h"
#include "prov_logic.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char ssid[PROV_SSID_MAX + 1];
    char psk[PROV_PSK_MAX + 2]; /* 64-char raw PSK plus terminator */
} prov_creds_t;

esp_err_t prov_nvs_save(const prov_creds_t *c);

/* False when nothing has been provisioned yet. */
bool prov_nvs_load(prov_creds_t *out);

esp_err_t prov_nvs_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* PROV_NVS_H */
