/*
 * Internal to wifi_prov: the radio, the DNS responder, the web server,
 * and the bridge the HTTP layer uses to reach the session. Not part of
 * the component's public surface.
 */
#ifndef PROV_AP_H
#define PROV_AP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

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

/* --------------------------------------------------- session bridge
 *
 * Implemented in wifi_prov.c, which owns the prov_logic context. The
 * HTTP layer never touches that context directly: every login attempt
 * goes through here so the failure counter and the session teardown
 * stay in one place.
 */

/* Checks the credentials and records the attempt. Five failures end
 * the session (ADR 0006). */
bool prov_session_login(const char *user, const char *password);

bool prov_session_is_open(void);
bool prov_session_is_authenticated(void);

/* Drops authentication without ending the session. */
void prov_session_logout(void);

/* The admin pressed "finish": close the portal now. */
void prov_session_finish(void);

#endif /* PROV_AP_H */
