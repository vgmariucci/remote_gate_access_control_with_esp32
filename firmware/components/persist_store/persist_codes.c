#include "persist_codes.h"

#include <string.h>

/*  0..3   magic
 *  4..7   generation
 *  8      version
 *  9      flags: bit 0 occupied
 * 10..18  id
 * 19      reserved
 * 20..51  hash
 * 52..55  valid_from
 * 56..59  valid_until
 * 60..61  crc16 over 0..59
 * 62..63  0xFF
 */
#define F_OCCUPIED 0x01
#define CRC_AT 60

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t crc16(const uint8_t *d, size_t n)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

void pc_encode(uint8_t out[PC_REC_SIZE], const pc_code_t *c, uint32_t generation)
{
    memset(out, 0xFF, PC_REC_SIZE);
    put32(&out[0], PC_MAGIC);
    put32(&out[4], generation);
    out[8] = PC_VERSION;
    out[9] = c->occupied ? F_OCCUPIED : 0x00;
    memset(&out[10], 0, 10);
    if (c->occupied) {
        memcpy(&out[10], c->id, AC_ID_LEN);
        memcpy(&out[20], c->hash, AC_HASH_LEN);
    } else {
        /* Erase, do not merely mark: a revoked code leaves no hash. */
        memset(&out[20], 0, AC_HASH_LEN);
    }
    put32(&out[52], c->occupied ? c->valid_from : 0);
    put32(&out[56], c->occupied ? c->valid_until : 0);
    uint16_t crc = crc16(out, CRC_AT);
    out[CRC_AT] = (uint8_t)crc;
    out[CRC_AT + 1] = (uint8_t)(crc >> 8);
}

bool pc_decode(const uint8_t in[PC_REC_SIZE], pc_code_t *c, uint32_t *generation)
{
    if (get32(&in[0]) != PC_MAGIC || in[8] != PC_VERSION) {
        return false;
    }
    uint16_t stored = (uint16_t)in[CRC_AT] | ((uint16_t)in[CRC_AT + 1] << 8);
    if (crc16(in, CRC_AT) != stored) {
        return false;
    }
    if (c != NULL) {
        memset(c, 0, sizeof(*c));
        c->occupied = (in[9] & F_OCCUPIED) != 0;
        if (c->occupied) {
            memcpy(c->id, &in[10], AC_ID_LEN);
            c->id[AC_ID_LEN - 1] = '\0';
            memcpy(c->hash, &in[20], AC_HASH_LEN);
            c->valid_from = get32(&in[52]);
            c->valid_until = get32(&in[56]);
        }
    }
    if (generation != NULL) {
        *generation = get32(&in[4]);
    }
    return true;
}

bool pc_slot_read(const uint8_t slot[PC_SLOT_SIZE], pc_code_t *c, uint32_t *generation)
{
    pc_code_t ca, cb;
    uint32_t ga = 0, gb = 0;
    bool a = pc_decode(&slot[0], &ca, &ga);
    bool b = pc_decode(&slot[PC_REC_SIZE], &cb, &gb);

    if (!a && !b) {
        return false;
    }
    /* Wrap-safe comparison: (int32_t)(ga - gb) > 0 means A is newer. */
    bool use_a = a && (!b || (int32_t)(ga - gb) > 0);
    if (c != NULL) {
        *c = use_a ? ca : cb;
    }
    if (generation != NULL) {
        *generation = use_a ? ga : gb;
    }
    return true;
}

int pc_slot_write_half(const uint8_t slot[PC_SLOT_SIZE], uint32_t *next_generation)
{
    uint32_t ga = 0, gb = 0;
    bool a = pc_decode(&slot[0], NULL, &ga);
    bool b = pc_decode(&slot[PC_REC_SIZE], NULL, &gb);

    uint32_t newest = 0;
    int half;
    if (!a && !b) {
        half = 0;
    } else if (a && !b) {
        half = 1;
        newest = ga;
    } else if (!a && b) {
        half = 0;
        newest = gb;
    } else {
        /* Overwrite the older one, keeping the newer as the fallback. */
        bool a_newer = (int32_t)(ga - gb) > 0;
        half = a_newer ? 1 : 0;
        newest = a_newer ? ga : gb;
    }
    if (next_generation != NULL) {
        *next_generation = newest + 1;
    }
    return half;
}
