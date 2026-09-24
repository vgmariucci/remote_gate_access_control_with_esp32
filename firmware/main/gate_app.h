/*
 * Shared state between app_main and the dev console, which run in
 * different tasks. Take the lock around any access to the contexts.
 */
#ifndef GATE_APP_H
#define GATE_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "access_core.h"
#include "driver/i2c_master.h"

void gate_app_lock(void);
void gate_app_unlock(void);
ac_ctx_t *gate_app_access(void);
void gate_app_set_clock_trusted(bool trusted);
void gate_app_hash(const char *code, uint8_t out[AC_HASH_LEN]);
void gate_app_show_border(uint32_t duration_ms);
i2c_master_bus_handle_t gate_app_bus(void);

#endif /* GATE_APP_H */
