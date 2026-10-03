/*
 * wifi_prov - the provisioning portal.
 *
 * Owns the button, the SoftAP, the captive-portal DNS responder and the
 * web server. Every rule about when the AP may be open lives in
 * prov_logic (ADR 0006) and is host-tested; this layer only turns those
 * decisions into radio and sockets.
 *
 * The caller polls wifi_prov_tick() from the main loop and acts on the
 * event it returns. Nothing here runs unless the button was held.
 */
#ifndef WIFI_PROV_H
#define WIFI_PROV_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "prov_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_PROV_EVT_NONE = 0,
    WIFI_PROV_EVT_OPENED,
    WIFI_PROV_EVT_CLOSED, /* caller revokes dev codes here */
} wifi_prov_evt_t;

/* Secrets are passed in rather than compiled into this component, so
 * they stay in main/admin_credentials.h, which is gitignored (ADR
 * 0006). Every pointer must outlive the component: string literals or
 * static buffers. */
typedef struct {
    gpio_num_t button_gpio;
    const char *ap_password;    /* WPA2 passphrase for the portal AP, 8-63 */
    const char *admin_user;     /* portal login */
    const char *admin_password; /* portal login */
} wifi_prov_config_t;

/* Configures the button pin and the network stack. Does not touch the
 * radio: an unprovisioned gate that nobody has pressed the button on
 * transmits nothing. */
esp_err_t wifi_prov_init(const wifi_prov_config_t *cfg);

/* Poll from the main loop. Reads the button, drives the session clock,
 * and starts or stops the AP when prov_logic says so. */
wifi_prov_evt_t wifi_prov_tick(uint32_t now_ms);

/* Ends the session early: the admin pressed finish, or the caller is
 * shutting down. */
void wifi_prov_close(void);

bool wifi_prov_is_open(void);
uint32_t wifi_prov_seconds_left(uint32_t now_ms);

/* The SSID the AP is advertising, for the display and the log. Empty
 * while closed. */
const char *wifi_prov_ssid(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_PROV_H */
