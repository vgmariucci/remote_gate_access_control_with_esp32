#include <stdio.h>
/*
 * Tests for transient dev codes and for persistent code records.
 *
 * The properties that matter are the ones about codes NOT surviving:
 * a dev code must not outlive its session, and must never reach the
 * EEPROM. And the one about codes surviving: a power cut mid-write must
 * leave the previous code intact.
 */
#include <string.h>

#include "persist_codes.h"
#include "unity.h"

#define T0 1790000000LL
#define DAY 86400LL

static ac_ctx_t ac;
static uint8_t eeprom[PC_SLOTS * PC_SLOT_SIZE];

static void hash_of(uint8_t out[AC_HASH_LEN], uint8_t seed)
{
    for (int i = 0; i < AC_HASH_LEN; i++) {
        out[i] = (uint8_t)(seed + i);
    }
}

static void fixture(void)
{
    ac_init(&ac, 5, 300);
    ac.clock_trusted = true;
    memset(eeprom, 0xFF, sizeof(eeprom)); /* erased chip */
}

/* ---------------------------------------------------------- transient */

TEST_CASE("the table holds five codes", "[codes]")
{
    fixture();
    uint8_t h[AC_HASH_LEN];
    char id[AC_ID_LEN];
    for (int i = 0; i < 5; i++) {
        hash_of(h, (uint8_t)i);
        snprintf(id, sizeof(id), "guest%03d", i);
        TEST_ASSERT_TRUE(ac_upsert(&ac, id, h, T0, T0 + DAY) >= 0);
    }
    TEST_ASSERT_EQUAL_UINT(5, ac_count(&ac));

    hash_of(h, 99);
    TEST_ASSERT_EQUAL_INT(-1, ac_upsert(&ac, "overflow", h, T0, T0 + DAY));
}

TEST_CASE("a dev code is capped at fifteen minutes", "[codes]")
{
    fixture();
    uint8_t h[AC_HASH_LEN];
    hash_of(h, 7);
    /* The console asks for a day; it gets the ceiling. */
    int slot = ac_upsert_transient(&ac, "dev00001", h, T0, T0 + DAY);
    TEST_ASSERT_TRUE(slot >= 0);
    TEST_ASSERT_EQUAL_INT64(T0 + AC_TRANSIENT_MAX_S, ac.slots[slot].valid_until);

    TEST_ASSERT_EQUAL(AC_GRANTED, ac_evaluate(&ac, h, T0 + 60, NULL));
    TEST_ASSERT_EQUAL(AC_DENIED_EXPIRED,
                      ac_evaluate(&ac, h, T0 + AC_TRANSIENT_MAX_S + 1, NULL));
}

TEST_CASE("a shorter dev code keeps its own window", "[codes]")
{
    fixture();
    uint8_t h[AC_HASH_LEN];
    hash_of(h, 8);
    int slot = ac_upsert_transient(&ac, "dev00002", h, T0, T0 + 600);
    TEST_ASSERT_EQUAL_INT64(T0 + 600, ac.slots[slot].valid_until);
}

TEST_CASE("ending a dev session revokes only the dev codes", "[codes]")
{
    fixture();
    uint8_t guest[AC_HASH_LEN], dev1[AC_HASH_LEN], dev2[AC_HASH_LEN];
    hash_of(guest, 10);
    hash_of(dev1, 20);
    hash_of(dev2, 30);

    ac_upsert(&ac, "guest001", guest, T0, T0 + DAY);
    ac_upsert_transient(&ac, "dev00001", dev1, T0, T0 + 600);
    ac_upsert_transient(&ac, "dev00002", dev2, T0, T0 + 600);
    TEST_ASSERT_EQUAL_UINT(3, ac_count(&ac));

    TEST_ASSERT_EQUAL_INT(2, ac_revoke_transient(&ac));
    TEST_ASSERT_EQUAL_UINT(1, ac_count(&ac));

    TEST_ASSERT_EQUAL(AC_GRANTED, ac_evaluate(&ac, guest, T0 + 60, NULL));
    TEST_ASSERT_EQUAL(AC_DENIED_UNKNOWN, ac_evaluate(&ac, dev1, T0 + 60, NULL));
    TEST_ASSERT_EQUAL(AC_DENIED_UNKNOWN, ac_evaluate(&ac, dev2, T0 + 60, NULL));

    /* Idempotent: the three triggers may fire in any order. */
    TEST_ASSERT_EQUAL_INT(0, ac_revoke_transient(&ac));
}

/* --------------------------------------------------------- persistence */

/* What the target glue does for one save. */
static void save_slot(int slot, const pc_code_t *c)
{
    uint8_t *img = &eeprom[slot * PC_SLOT_SIZE];
    uint32_t gen;
    int half = pc_slot_write_half(img, &gen);
    pc_encode(&img[half * PC_REC_SIZE], c, gen);
}

static pc_code_t a_code(const char *id, uint8_t seed, uint32_t until)
{
    pc_code_t c;
    memset(&c, 0, sizeof(c));
    c.occupied = true;
    snprintf(c.id, sizeof(c.id), "%s", id);
    hash_of(c.hash, seed);
    c.valid_from = (uint32_t)T0;
    c.valid_until = until;
    return c;
}

TEST_CASE("a code record survives a round trip", "[codes]")
{
    fixture();
    pc_code_t in = a_code("guest001", 42, (uint32_t)(T0 + DAY)), out;
    uint8_t rec[PC_REC_SIZE];
    uint32_t gen;
    pc_encode(rec, &in, 9);
    TEST_ASSERT_TRUE(pc_decode(rec, &out, &gen));
    TEST_ASSERT_TRUE(out.occupied);
    TEST_ASSERT_EQUAL_STRING("guest001", out.id);
    TEST_ASSERT_EQUAL_MEMORY(in.hash, out.hash, AC_HASH_LEN);
    TEST_ASSERT_EQUAL_UINT32(in.valid_until, out.valid_until);
    TEST_ASSERT_EQUAL_UINT32(9, gen);
}

TEST_CASE("an erased chip holds no codes", "[codes]")
{
    fixture();
    for (int i = 0; i < PC_SLOTS; i++) {
        TEST_ASSERT_FALSE(pc_slot_read(&eeprom[i * PC_SLOT_SIZE], NULL, NULL));
    }
}

TEST_CASE("writes alternate between the two buffers", "[codes]")
{
    fixture();
    /* First write lands in A, second in B, third back in A. */
    pc_code_t c = a_code("guest001", 1, (uint32_t)(T0 + DAY));
    TEST_ASSERT_EQUAL_INT(0, pc_slot_write_half(&eeprom[0], NULL));
    save_slot(0, &c);
    TEST_ASSERT_EQUAL_INT(1, pc_slot_write_half(&eeprom[0], NULL));
    save_slot(0, &c);
    TEST_ASSERT_EQUAL_INT(0, pc_slot_write_half(&eeprom[0], NULL));
}

TEST_CASE("the newer buffer wins", "[codes]")
{
    fixture();
    pc_code_t first = a_code("guest001", 1, (uint32_t)(T0 + DAY));
    pc_code_t second = a_code("guest002", 2, (uint32_t)(T0 + 2 * DAY));
    save_slot(0, &first);
    save_slot(0, &second);

    pc_code_t got;
    TEST_ASSERT_TRUE(pc_slot_read(&eeprom[0], &got, NULL));
    TEST_ASSERT_EQUAL_STRING("guest002", got.id);
}

TEST_CASE("a power cut mid-write leaves the previous code intact", "[codes]")
{
    fixture();
    pc_code_t first = a_code("guest001", 1, (uint32_t)(T0 + DAY));
    pc_code_t second = a_code("guest002", 2, (uint32_t)(T0 + 2 * DAY));
    save_slot(0, &first);

    /* The second write reached the first EEPROM page and stopped. */
    int half = pc_slot_write_half(&eeprom[0], NULL);
    uint8_t *target = &eeprom[half * PC_REC_SIZE];
    uint8_t full[PC_REC_SIZE];
    uint32_t gen;
    pc_slot_write_half(&eeprom[0], &gen);
    pc_encode(full, &second, gen);
    memcpy(target, full, 32);      /* page 0 landed */
    memset(target + 32, 0xFF, 32); /* page 1 never did */

    pc_code_t got;
    TEST_ASSERT_TRUE(pc_slot_read(&eeprom[0], &got, NULL));
    TEST_ASSERT_EQUAL_STRING("guest001", got.id);
    TEST_ASSERT_EQUAL_MEMORY(first.hash, got.hash, AC_HASH_LEN);
}

TEST_CASE("a revoked code is erased, not just unflagged", "[codes]")
{
    fixture();
    pc_code_t c = a_code("guest001", 77, (uint32_t)(T0 + DAY));
    save_slot(0, &c);

    pc_code_t empty;
    memset(&empty, 0, sizeof(empty));
    save_slot(0, &empty);

    pc_code_t got;
    TEST_ASSERT_TRUE(pc_slot_read(&eeprom[0], &got, NULL));
    TEST_ASSERT_FALSE(got.occupied);

    /* No trace of the hash anywhere in the newer buffer. */
    uint8_t stale[AC_HASH_LEN];
    hash_of(stale, 77);
    int half = pc_slot_write_half(&eeprom[0], NULL) == 0 ? 1 : 0;
    const uint8_t *newer = &eeprom[half * PC_REC_SIZE];
    bool found = false;
    for (int i = 0; i + AC_HASH_LEN <= PC_REC_SIZE; i++) {
        if (memcmp(&newer[i], stale, AC_HASH_LEN) == 0) {
            found = true;
        }
    }
    TEST_ASSERT_FALSE(found);
}

TEST_CASE("dev ids and backend ids cannot be confused", "[codes]")
{
    /* Backend ids are 8 lowercase hex characters. */
    TEST_ASSERT_FALSE(ac_id_is_dev("a1b2c3d4"));
    TEST_ASSERT_FALSE(ac_id_is_dev("00000000"));
    TEST_ASSERT_FALSE(ac_id_is_dev("ffffffff"));
    TEST_ASSERT_FALSE(ac_id_is_dev("deadbeef"));

    /* Dev ids always contain a non-hex character, so the sets are
     * disjoint by construction rather than by convention. */
    TEST_ASSERT_TRUE(ac_id_is_dev("dev00001")); /* 'v' */
    TEST_ASSERT_TRUE(ac_id_is_dev("tab9ae0b")); /* 't' */
    TEST_ASSERT_TRUE(ac_id_is_dev("tst00001")); /* 't', 's' */

    /* Anything malformed is treated as dev, never as a guest's: the
     * safe direction is refusing to erase. */
    TEST_ASSERT_TRUE(ac_id_is_dev(""));
    TEST_ASSERT_TRUE(ac_id_is_dev(NULL));
    TEST_ASSERT_TRUE(ac_id_is_dev("a1b2c3"));    /* too short */
    TEST_ASSERT_TRUE(ac_id_is_dev("a1b2c3d4e")); /* too long */
    TEST_ASSERT_TRUE(ac_id_is_dev("A1B2C3D4"));  /* uppercase */
}

TEST_CASE("a restored code lands in the slot it came from", "[codes]")
{
    fixture();
    pc_code_t c = a_code("guest003", 33, (uint32_t)(T0 + DAY));
    TEST_ASSERT_TRUE(pc_apply(&ac, 3, &c));
    TEST_ASSERT_TRUE(ac.slots[3].occupied);
    TEST_ASSERT_FALSE(ac.slots[0].occupied);
    TEST_ASSERT_EQUAL_STRING("guest003", ac.slots[3].id);
    TEST_ASSERT_EQUAL_MEMORY(c.hash, ac.slots[3].hash, AC_HASH_LEN);
}

TEST_CASE("a restored code is never transient", "[codes]")
{
    fixture();
    /* Anything on the chip is a stored code, and must not be swept
     * away by the next `dev off`. */
    uint8_t h[AC_HASH_LEN];
    hash_of(h, 1);
    ac_upsert_transient(&ac, "dev00001", h, T0, T0 + 600);

    pc_code_t c = a_code("guest000", 5, (uint32_t)(T0 + DAY));
    TEST_ASSERT_TRUE(pc_apply(&ac, 0, &c));
    TEST_ASSERT_FALSE(ac.slots[0].transient);
    TEST_ASSERT_EQUAL_INT(0, ac_revoke_transient(&ac));
    TEST_ASSERT_TRUE(ac.slots[0].occupied);
}

TEST_CASE("records sharing an id do not collapse into one slot", "[codes]")
{
    fixture();
    /* The console numbered stored codes with a counter that reset at
     * boot, so two records could carry the same id. Restoring by slot
     * index keeps both; restoring through ac_upsert lost one. */
    pc_code_t first = a_code("tst00001", 1, (uint32_t)(T0 + DAY));
    pc_code_t second = a_code("tst00001", 2, (uint32_t)(T0 + DAY));
    TEST_ASSERT_TRUE(pc_apply(&ac, 0, &first));
    TEST_ASSERT_TRUE(pc_apply(&ac, 1, &second));
    TEST_ASSERT_EQUAL_UINT(2, ac_count(&ac));
    TEST_ASSERT_EQUAL_MEMORY(first.hash, ac.slots[0].hash, AC_HASH_LEN);
    TEST_ASSERT_EQUAL_MEMORY(second.hash, ac.slots[1].hash, AC_HASH_LEN);
}

TEST_CASE("an empty record restores nothing", "[codes]")
{
    fixture();
    pc_code_t empty;
    memset(&empty, 0, sizeof(empty));
    TEST_ASSERT_FALSE(pc_apply(&ac, 2, &empty));
    TEST_ASSERT_FALSE(ac.slots[2].occupied);
}

TEST_CASE("the code region does not collide with the attempt ring", "[codes]")
{
    /* The ring owns 0x000-0x7FF; codes must start after it and fit. */
    TEST_ASSERT_EQUAL_UINT16(0x800, pc_offset(0, 0));
    TEST_ASSERT_TRUE(pc_offset(PC_SLOTS - 1, 1) + PC_REC_SIZE <= 4096);
    TEST_ASSERT_EQUAL_UINT16(0x800 + PC_SLOT_SIZE, pc_offset(1, 0));
}
