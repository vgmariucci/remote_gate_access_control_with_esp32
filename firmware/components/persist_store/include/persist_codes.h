/*
 * persist_codes - guest codes that survive a power cut.
 *
 * ADR 0001 says the gate validates offline. Until now the code table
 * lived only in RAM, so a power cut left a guest at the door with a
 * valid code the lock had forgotten. These records fix that.
 *
 * Pure C99: the record format and the A/B buffer choice are here and
 * host-tested; the I2C transport is in persist_store.c.
 *
 * EEPROM map (AT24C32, 4 KB):
 *   0x000-0x7FF  attempt-counter ring, 64 slots x 32 B
 *   0x800-0xA7F  5 code slots x 128 B  (two 64-byte buffers each)
 *   0xA80-0xFFF  spare
 *
 * A record is 64 bytes: two EEPROM pages, so a write can be cut in
 * half between them. Each slot therefore holds two buffers written
 * alternately with a rising generation number. Load takes the newer
 * valid one, so an interrupted write always leaves the previous code
 * intact — the same guarantee the attempt ring has.
 *
 * The hash is stored in plain sight on a chip anyone with a $2 adapter
 * can read. The salt lives in the ESP32's flash, and both are needed to
 * recover a code — but an attacker holding the board can open the door
 * with a screwdriver, so this gives up nothing that mattered.
 */
#ifndef PERSIST_CODES_H
#define PERSIST_CODES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "access_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PC_REC_SIZE 64
#define PC_SLOT_SIZE (2 * PC_REC_SIZE) /* buffer A + buffer B */
#define PC_SLOTS AC_MAX_SLOTS
#define PC_BASE 0x800
#define PC_MAGIC 0x45444F43u /* "CODE" */
#define PC_VERSION 1

typedef struct {
    bool occupied;
    char id[AC_ID_LEN];
    uint8_t hash[AC_HASH_LEN];
    uint32_t valid_from;
    uint32_t valid_until;
} pc_code_t;

/* A free slot is written as an occupied=false record rather than left
 * alone, so a revoked code is positively erased instead of lingering. */
void pc_encode(uint8_t out[PC_REC_SIZE], const pc_code_t *c, uint32_t generation);
bool pc_decode(const uint8_t in[PC_REC_SIZE], pc_code_t *c, uint32_t *generation);

/* Picks the newer valid buffer of a slot image. Returns false when
 * neither is valid (blank chip, or both corrupted). */
bool pc_slot_read(const uint8_t slot[PC_SLOT_SIZE], pc_code_t *c, uint32_t *generation);

/* Which half to write next, given the slot as it stands: the older or
 * invalid buffer, never the one currently holding the good copy. */
int pc_slot_write_half(const uint8_t slot[PC_SLOT_SIZE], uint32_t *next_generation);

/* Byte offset of a slot's buffer within the EEPROM. */
static inline uint16_t pc_offset(int slot, int half)
{
    return (uint16_t)(PC_BASE + slot * PC_SLOT_SIZE + half * PC_REC_SIZE);
}

#ifdef __cplusplus
}
#endif

#endif /* PERSIST_CODES_H */
