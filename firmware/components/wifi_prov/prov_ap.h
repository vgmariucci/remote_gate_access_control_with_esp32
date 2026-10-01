/*
 * Internal to wifi_prov: the radio, the DNS responder and the web
 * server. Not part of the component's public surface.
 */
#ifndef PROV_AP_H
#define PROV_AP_H

#include <stddef.h>

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define PROV_AP_SSID_MAX 33
#define PROV_AP_IP "192.168.4.1"

/* Brings up the SoftAP with WPA2, the DNS responder and the HTTP
 * server. The SSID is written into `out_ssid`. */
esp_err_t prov_ap_start(const char *password, char *out_ssid, size_t out_len);

/* Tears all three down. Safe to call when not started. */
void prov_ap_stop(void);

/* Answers every A query with our own address, so any hostname a phone
 * tries lands on the portal and the "sign in to network" prompt
 * appears. */
esp_err_t prov_dns_start(void);
void prov_dns_stop(void);

esp_err_t prov_http_start(void);
void prov_http_stop(void);

#endif /* PROV_AP_H */
