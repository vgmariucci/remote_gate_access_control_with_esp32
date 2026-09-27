#include "persist_store.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "persist";

#define XFER_TIMEOUT_MS 50
#define READ_CHUNK 256    /* bytes per sequential read */
#define WRITE_CYCLE_MS 10 /* AT24C32 datasheet maximum */
#define PAGE_SIZE 32

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint8_t s_addr;
static pr_cursor_t s_cur = {.slot = -1, .seq = 0};
static pr_state_t s_found;
static bool s_have;

/* The chip ignores the bus while it commits a page. Poll for its ACK
 * rather than sleeping the worst case every time. */
static esp_err_t wait_write_cycle(void)
{
    for (int i = 0; i < WRITE_CYCLE_MS * 2; i++) {
        if (i2c_master_probe(s_bus, s_addr, 5) == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(1);
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t persist_raw_read(uint16_t mem, uint8_t *buf, size_t len)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t a[2] = {(uint8_t)(mem >> 8), (uint8_t)mem};
    return i2c_master_transmit_receive(s_dev, a, sizeof(a), buf, len, XFER_TIMEOUT_MS);
}

esp_err_t persist_raw_write(uint16_t mem, const uint8_t *buf, size_t len)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len > PAGE_SIZE) {
        return ESP_ERR_INVALID_SIZE; /* one page per call */
    }
    uint8_t tmp[2 + PAGE_SIZE];
    tmp[0] = (uint8_t)(mem >> 8);
    tmp[1] = (uint8_t)mem;
    memcpy(&tmp[2], buf, len);
    esp_err_t err = i2c_master_transmit(s_dev, tmp, len + 2, XFER_TIMEOUT_MS);
    return err == ESP_OK ? wait_write_cycle() : err;
}

esp_err_t persist_init(i2c_master_bus_handle_t bus, uint8_t addr)
{
    s_bus = bus;
    s_addr = addr;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        return err;
    }

    /* The ring only: 2 KB, into a static buffer rather than the stack. */
    static uint8_t image[PR_SLOTS * PR_SLOT_SIZE];
    for (size_t off = 0; off < sizeof(image); off += READ_CHUNK) {
        err = persist_raw_read((uint16_t)off, &image[off], READ_CHUNK);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "EEPROM at 0x%02X not answering", addr);
            return err;
        }
    }

    s_have = pr_find_latest(image, PR_SLOTS, &s_found, &s_cur);
    if (s_have) {
        ESP_LOGI(TAG, "record seq %u in slot %d", (unsigned)s_cur.seq, s_cur.slot);
    } else {
        ESP_LOGI(TAG, "no record yet: starting clean");
    }
    return ESP_OK;
}

bool persist_load(pr_state_t *out)
{
    if (s_have && out != NULL) {
        *out = s_found;
    }
    return s_have;
}

esp_err_t persist_save(const pr_state_t *s)
{
    if (s_dev == NULL || s == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    pr_cursor_t next = s_cur;
    int slot = pr_advance(&next, PR_SLOTS);

    uint8_t rec[PR_SLOT_SIZE];
    pr_encode(rec, s, next.seq);

    esp_err_t err = persist_raw_write((uint16_t)(slot * PR_SLOT_SIZE), rec, PR_SLOT_SIZE);
    /* Advance only on success: a failed write leaves the cursor where it
     * was, so the next attempt retries the same slot. */
    if (err == ESP_OK) {
        s_cur = next;
    }
    return err;
}

/* ------------------------------------------------------------ codes */

int persist_codes_load(ac_ctx_t *ctx)
{
    if (ctx == NULL || s_dev == NULL) {
        return 0;
    }
    int loaded = 0;
    for (int i = 0; i < PC_SLOTS; i++) {
        uint8_t slot[PC_SLOT_SIZE];
        if (persist_raw_read(pc_offset(i, 0), slot, sizeof(slot)) != ESP_OK) {
            ESP_LOGW(TAG, "code slot %d unreadable", i);
            continue;
        }
        pc_code_t c;
        if (!pc_slot_read(slot, &c, NULL) || !c.occupied) {
            continue;
        }
        if (ac_upsert(ctx, c.id, c.hash, (int64_t)c.valid_from, (int64_t)c.valid_until) >= 0) {
            loaded++;
        }
    }
    ESP_LOGI(TAG, "%d code(s) restored from EEPROM", loaded);
    return loaded;
}

esp_err_t persist_codes_save(const ac_ctx_t *ctx, int slot)
{
    if (ctx == NULL || slot < 0 || slot >= PC_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }
    const ac_slot_t *s = &ctx->slots[slot];

    /* A transient dev code never reaches the chip: that is what makes
     * "revoked when the session ends" structural rather than a rule
     * someone has to remember. */
    if (s->occupied && s->transient) {
        return ESP_OK;
    }

    uint8_t img[PC_SLOT_SIZE];
    esp_err_t err = persist_raw_read(pc_offset(slot, 0), img, sizeof(img));
    if (err != ESP_OK) {
        return err;
    }
    uint32_t gen;
    int half = pc_slot_write_half(img, &gen);

    pc_code_t c;
    memset(&c, 0, sizeof(c));
    if (s->occupied) {
        c.occupied = true;
        memcpy(c.id, s->id, AC_ID_LEN);
        memcpy(c.hash, s->hash, AC_HASH_LEN);
        c.valid_from = (uint32_t)s->valid_from;
        c.valid_until = (uint32_t)s->valid_until;
    }

    uint8_t rec[PC_REC_SIZE];
    pc_encode(rec, &c, gen);

    /* Two EEPROM pages. A cut between them leaves this buffer invalid
     * and the other one still holding the previous code. */
    uint16_t at = pc_offset(slot, half);
    err = persist_raw_write(at, rec, PAGE_SIZE);
    if (err == ESP_OK) {
        err = persist_raw_write((uint16_t)(at + PAGE_SIZE), rec + PAGE_SIZE, PAGE_SIZE);
    }
    return err;
}
