/*
 * persist_store - non-volatile state on the ZS-042 board's AT24C32.
 *
 * The DS3231M has no SRAM (ADR 0004, amended), so everything that must
 * survive a power cut lives in the EEPROM that shares its board and bus.
 *
 * EEPROM map (4 KB):
 *   0x000-0x7FF  attempt-counter ring, 64 slots x 32 B   (persist_ring)
 *   0x800-0xA7F  5 code slots x 128 B                    (persist_codes)
 *   0xA80-0xFFF  spare
 */
#ifndef PERSIST_STORE_H
#define PERSIST_STORE_H

#include "access_core.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "persist_codes.h"
#include "persist_ring.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 7-bit address. The ZS-042 pulls A0-A2 high, giving 0x57. Confirm
 * with `i2c scan` on the dev console. */
#define PERSIST_AT24C32_ADDR 0x57

/* Adds the device and reads the attempt ring once. */
esp_err_t persist_init(i2c_master_bus_handle_t bus, uint8_t addr);

/* True when a valid attempt record was found at init. */
bool persist_load(pr_state_t *out);

/* Writes the attempt counter to the next slot in the ring. */
esp_err_t persist_save(const pr_state_t *s);

/* Restores stored guest codes into the table. Returns how many. */
int persist_codes_load(ac_ctx_t *ctx);

/* Writes one code slot through. A transient slot is skipped, which is
 * what makes "dev codes never persist" structural. */
esp_err_t persist_codes_save(const ac_ctx_t *ctx, int slot);

/* Raw EEPROM access, shared by both regions. A write must not cross a
 * 32-byte page boundary, so len <= 32 and mem must be page-aligned for
 * multi-byte writes. */
esp_err_t persist_raw_read(uint16_t mem, uint8_t *buf, size_t len);
esp_err_t persist_raw_write(uint16_t mem, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* PERSIST_STORE_H */
