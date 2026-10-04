/*
 * prov_nvs - the Wi-Fi credentials, as stored.
 *
 * Lives with net_link rather than with the portal: the station reads
 * these at every boot, while the portal only writes them occasionally.
 *
 * The sizes are the 802.11 limits, stated here rather than borrowed
 * from prov_logic so that the link layer does not depend on the
 * provisioning layer.
 */
#ifndef PROV_NVS_H
#define PROV_NVS_H

#include <stdbool.h>

#include "esp_err.h"

#define PROV_NVS_SSID_MAX 32
#define PROV_NVS_PSK_MAX 64

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char ssid[PROV_NVS_SSID_MAX + 1];
    char psk[PROV_NVS_PSK_MAX + 1];
} prov_creds_t;

esp_err_t prov_nvs_save(const prov_creds_t *c);

/* False when nothing has been provisioned yet. */
bool prov_nvs_load(prov_creds_t *out);

esp_err_t prov_nvs_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* PROV_NVS_H */
