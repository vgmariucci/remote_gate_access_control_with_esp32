/*
 * rtc_ds3231 - trusted time from a DS3231 / DS3231M.
 *
 * Timekeeping only. The DS3231 family has no general-purpose SRAM, so
 * the attempt counter lives in persist_store (ADR 0004, amended).
 *
 * clock_trusted requires the oscillator-stop flag (OSF, status register
 * bit 7) to be clear AND the year to be plausible. OSF latches on any
 * loss of both VCC and VBAT, which is exactly the case where the time
 * cannot be believed. The register map below 0x13 is identical on the
 * DS3231, DS3231M and DS3232, so this driver serves all three.
 */
#ifndef RTC_DS3231_H
#define RTC_DS3231_H

#include "driver/i2c_master.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DS3231_I2C_ADDR 0x68

#define RTC_MIN_PLAUSIBLE_EPOCH 1767225600LL /* 2026-01-01 UTC */
#define RTC_MAX_PLAUSIBLE_EPOCH 2524608000LL /* 2050-01-01 UTC */

esp_err_t rtc_ds3231_init(i2c_master_bus_handle_t bus);

/* ESP_ERR_INVALID_STATE when OSF is set or the reading is implausible;
 * *out_epoch is untouched then. */
esp_err_t rtc_ds3231_get_time(int64_t *out_epoch);

/* Writes the time and clears OSF. */
esp_err_t rtc_ds3231_set_time(int64_t epoch);

bool rtc_ds3231_clock_trusted(void);

#ifdef __cplusplus
}
#endif

#endif /* RTC_DS3231_H */
