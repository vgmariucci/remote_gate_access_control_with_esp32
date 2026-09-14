/*
 * rtc_ds3232 - trusted time, plus the tamper-resistant attempt counter.
 *
 * Two jobs, both security-relevant:
 *
 * 1. Produce the clock_trusted flag that access_core already depends on.
 *    A controller that cannot tell the time cannot enforce an expiry, so
 *    it must refuse every code rather than accept them. Trust requires
 *    the oscillator-stop flag (OSF, status register bit 7) to be clear
 *    AND the year to be plausible. OSF latches on any loss of both VCC
 *    and VBAT, which is exactly the case where the time is garbage.
 *
 * 2. Hold the failed-attempt counter in the DS3232's 236 bytes of
 *    CR2032-backed SRAM. If that counter lived in RAM, pulling the fuse
 *    would reset the five-attempt lockout and make it decorative. NVS
 *    would survive too, but at the cost of flash wear on every wrong
 *    guess, and flash is the one thing here we cannot replace in the
 *    field.
 */
#ifndef RTC_DS3232_H
#define RTC_DS3232_H

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DS3232_I2C_ADDR   0x68
#define DS3232_SRAM_BASE  0x14  /* 0x14..0xFF, 236 bytes */
#define DS3232_SRAM_SIZE  236

/* Anything outside this range means the reading is not believable. */
#define RTC_MIN_PLAUSIBLE_EPOCH 1767225600LL /* 2026-01-01 UTC */
#define RTC_MAX_PLAUSIBLE_EPOCH 2524608000LL /* 2050-01-01 UTC */

typedef struct {
    uint8_t  attempts_used;
    uint32_t lockout_until_epoch; /* 0 = not locked out */
    uint32_t boot_count;
} rtc_persist_t;

esp_err_t rtc_ds3232_init(i2c_master_bus_handle_t bus);

/* Reads the RTC. Returns ESP_ERR_INVALID_STATE when OSF is set or the
 * value is outside the plausible range; *out_epoch is untouched then. */
esp_err_t rtc_ds3232_get_time(int64_t *out_epoch);

/* Writes the RTC and clears OSF. Only ever called with a value that came
 * from SNTP. */
esp_err_t rtc_ds3232_set_time(int64_t epoch);

/* True once a plausible, non-OSF reading has been obtained since boot. */
bool rtc_ds3232_clock_trusted(void);

/*
 * One-shot SNTP. Brings up SNTP, waits up to timeout_ms for a sync, and
 * on success writes the result into the DS3232 and clears OSF, then shuts
 * SNTP down. One shot rather than a daemon because the DS3232 drifts
 * about 1 minute per year; polling a time server every hour buys nothing
 * and adds a failure mode on a device whose whole point is working
 * offline.
 */
esp_err_t rtc_ds3232_sync_ntp_once(uint32_t timeout_ms);

/* Battery-backed attempt state. */
esp_err_t rtc_ds3232_load_persist(rtc_persist_t *out);
esp_err_t rtc_ds3232_store_persist(const rtc_persist_t *in);

#ifdef __cplusplus
}
#endif

#endif /* RTC_DS3232_H */
