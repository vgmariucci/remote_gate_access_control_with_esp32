/*
 * net_link - the radio.
 *
 * One owner for esp_wifi. The station (the house network, for codes
 * and the clock) and the provisioning AP both run through here,
 * because they share one radio and only one piece of code can
 * initialise it.
 *
 * The radio stays off until something asks for it: an unprovisioned
 * gate that nobody has pressed the button on still transmits nothing.
 * Once credentials exist the station stays associated, which is what
 * tg_client needs (ADR 0006, amended).
 *
 * Nothing here gates the lock. Validation is offline (ADR 0001): a
 * gate with no network still opens for a valid code.
 */
#ifndef NET_LINK_H
#define NET_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called from the SNTP task when the network has supplied a time worth
 * believing. The caller writes it to the RTC and decides whether the
 * clock is now trusted; net_link has no opinion about that. */
typedef void (*net_link_time_cb)(int64_t epoch);

/* Brings up NVS, netif and the event loop. Does not touch the radio. */
esp_err_t net_link_init(net_link_time_cb on_time);

/* Starts the station with the stored credentials, if any. Returns
 * ESP_ERR_NOT_FOUND when nothing has been provisioned, which is not an
 * error worth stopping for. */
esp_err_t net_link_sta_start(void);

/* Stops trying, and drops the association. */
void net_link_sta_stop(void);

/* Drives the retry backoff. Call from the main loop. */
void net_link_tick(uint32_t now_ms);

bool net_link_sta_connected(void);

/* Dotted-quad address, or "-" when not connected. */
const char *net_link_sta_ip(void);

/* ------------------------------------------------------------- AP */

/* Raises the provisioning AP alongside whatever the station is doing.
 * `out_ssid` receives the generated name. */
esp_err_t net_link_ap_up(const char *password, char *out_ssid, size_t out_len);

/* Drops the AP. The station, if running, is left alone. */
void net_link_ap_down(void);

/* ----------------------------------------------------- scan, verify */

/* Scans for networks. `count` is in-out: capacity on the way in, found
 * on the way out. */
esp_err_t net_link_scan(wifi_ap_record_t *out, uint16_t *count);

/* Tries credentials and waits for an address, up to `timeout_ms`.
 *
 * This is verify-before-save: it answers "would these work?" without
 * writing anything. Whatever the station was doing before is restored
 * afterwards, so a failed test does not cost the gate its connection.
 */
esp_err_t net_link_try_credentials(const char *ssid, const char *psk, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* NET_LINK_H */
