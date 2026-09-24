/*
 * Unity tests for persist_ring. The EEPROM is simulated as a byte
 * image; power cuts are simulated by corrupting the slot being written.
 */
#include <string.h>

#include "persist_ring.h"
#include "unity.h"

static uint8_t eeprom[PR_SLOTS * PR_SLOT_SIZE];

static void blank(void)
{
    memset(eeprom, 0xFF, sizeof(eeprom)); /* an erased AT24C32 */
}

/* The same sequence of calls the target glue makes on every save. */
static void save(pr_cursor_t *cur, const pr_state_t *s)
{
    int slot = pr_advance(cur, PR_SLOTS);
    pr_encode(&eeprom[slot * PR_SLOT_SIZE], s, cur->seq);
}

static pr_state_t state(uint8_t attempts, uint32_t lockout, uint32_t boot)
{
    pr_state_t s = {.attempts = attempts, .lockout_until = lockout, .boot_count = boot};
    return s;
}

TEST_CASE("a record survives an encode and decode round trip", "[persist]")
{
    uint8_t slot[PR_SLOT_SIZE];
    pr_state_t in = state(3, 1773360300u, 42), out;
    uint32_t seq;
    pr_encode(slot, &in, 7);
    TEST_ASSERT_TRUE(pr_decode(slot, &out, &seq));
    TEST_ASSERT_EQUAL_UINT8(3, out.attempts);
    TEST_ASSERT_EQUAL_UINT32(1773360300u, out.lockout_until);
    TEST_ASSERT_EQUAL_UINT32(42, out.boot_count);
    TEST_ASSERT_EQUAL_UINT32(7, seq);
}

TEST_CASE("a blank chip reports nothing found", "[persist]")
{
    blank();
    pr_state_t s;
    pr_cursor_t cur;
    TEST_ASSERT_FALSE(pr_find_latest(eeprom, PR_SLOTS, &s, &cur));
    TEST_ASSERT_EQUAL_INT(-1, cur.slot);
}

TEST_CASE("load returns the newest of many saves", "[persist]")
{
    blank();
    pr_cursor_t cur = {.slot = -1, .seq = 0};
    for (uint8_t i = 1; i <= 5; i++) {
        pr_state_t s = state(i, 0, 1);
        save(&cur, &s);
    }
    pr_state_t got;
    pr_cursor_t found;
    TEST_ASSERT_TRUE(pr_find_latest(eeprom, PR_SLOTS, &got, &found));
    TEST_ASSERT_EQUAL_UINT8(5, got.attempts);
    TEST_ASSERT_EQUAL_INT(4, found.slot);
}

TEST_CASE("a single flipped bit invalidates a record", "[persist]")
{
    uint8_t slot[PR_SLOT_SIZE];
    pr_state_t s = state(2, 0, 1);
    pr_encode(slot, &s, 1);
    slot[9] ^= 0x01; /* attempts 2 -> 3 */
    TEST_ASSERT_FALSE(pr_decode(slot, NULL, NULL));
}

TEST_CASE("a torn write falls back to the previous record", "[persist]")
{
    blank();
    pr_cursor_t cur = {.slot = -1, .seq = 0};
    pr_state_t two = state(2, 0, 1), three = state(3, 0, 1);
    save(&cur, &two);
    save(&cur, &three);

    /* Power died while the second record was half written: the first
     * ten bytes landed, the rest is still whatever was there before. */
    memset(&eeprom[1 * PR_SLOT_SIZE + 10], 0xFF, PR_SLOT_SIZE - 10);

    pr_state_t got;
    TEST_ASSERT_TRUE(pr_find_latest(eeprom, PR_SLOTS, &got, NULL));
    TEST_ASSERT_EQUAL_UINT8(2, got.attempts);
}

TEST_CASE("writes wrap around the ring", "[persist]")
{
    blank();
    pr_cursor_t cur = {.slot = -1, .seq = 0};
    for (int i = 0; i < PR_SLOTS + 3; i++) {
        pr_state_t s = state((uint8_t)(i % 250), 0, (uint32_t)i);
        save(&cur, &s);
    }
    /* 131 saves into 128 slots: the newest sits in slot 2. */
    pr_state_t got;
    pr_cursor_t found;
    TEST_ASSERT_TRUE(pr_find_latest(eeprom, PR_SLOTS, &got, &found));
    TEST_ASSERT_EQUAL_INT(2, found.slot);
    TEST_ASSERT_EQUAL_UINT32(PR_SLOTS + 2, got.boot_count);
}

TEST_CASE("a reboot resumes the sequence rather than restarting it", "[persist]")
{
    blank();
    pr_cursor_t cur = {.slot = -1, .seq = 0};
    for (int i = 0; i < 10; i++) {
        pr_state_t s = state(1, 0, 1);
        save(&cur, &s);
    }

    /* Reboot: rediscover the cursor from the chip alone. */
    pr_cursor_t found;
    TEST_ASSERT_TRUE(pr_find_latest(eeprom, PR_SLOTS, NULL, &found));
    pr_state_t after = state(4, 0, 2);
    save(&found, &after);

    pr_state_t got;
    TEST_ASSERT_TRUE(pr_find_latest(eeprom, PR_SLOTS, &got, NULL));
    TEST_ASSERT_EQUAL_UINT8(4, got.attempts);
    TEST_ASSERT_EQUAL_UINT32(11, found.seq);
}

TEST_CASE("wear spreads evenly across every slot", "[persist]")
{
    static int writes[PR_SLOTS];
    memset(writes, 0, sizeof(writes));
    pr_cursor_t cur = {.slot = -1, .seq = 0};
    const int total = 100000;
    for (int i = 0; i < total; i++) {
        writes[pr_advance(&cur, PR_SLOTS)]++;
    }
    int lo = writes[0], hi = writes[0];
    for (int i = 1; i < PR_SLOTS; i++) {
        lo = writes[i] < lo ? writes[i] : lo;
        hi = writes[i] > hi ? writes[i] : hi;
    }
    /* Round-robin: no slot takes more than one write beyond another. */
    TEST_ASSERT_TRUE(hi - lo <= 1);
}
