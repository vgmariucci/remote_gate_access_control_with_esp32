/*
 * persist_store - the attempt counter on the ZS-042 board's AT24C32.
 *
 * The DS3231M has no SRAM (ADR 0004, amended), so the counter lives in
 * the EEPROM that shares its board and bus. persist_ring decides where
 * each record goes; this file moves bytes over I2C.
 */
#ifndef PERSIST_STORE_H
#define PERSIST_STORE_H

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "persist_ring.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 7-bit address. The ZS-042 pulls A0-A2 high, giving 0x57. Confirm
 * with `i2c scan` on the dev console. */
#define PERSIST_AT24C32_ADDR 0x57

/* Reads the whole ring once and locates the newest record. */
esp_err_t persist_init(i2c_master_bus_handle_t bus, uint8_t addr);

/* True when a valid record was found at init. */
bool persist_load(pr_state_t *out);

/* Writes to the next slot in the ring. */
esp_err_t persist_save(const pr_state_t *s);

#ifdef __cplusplus
}
#endif

#endif /* PERSIST_STORE_H */
