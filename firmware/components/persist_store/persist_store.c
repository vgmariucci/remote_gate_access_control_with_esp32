#include "persist_store.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "persist";

#define XFER_TIMEOUT_MS 50
#define READ_CHUNK 256    /* bytes per sequential read */
#define WRITE_CYCLE_MS 10 /* AT24C32 datasheet maximum */

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint8_t s_addr;
static pr_cursor_t s_cur = {.slot = -1, .seq = 0};
static pr_state_t s_found;
static bool s_have;

static esp_err_t eeprom_read(uint16_t mem, uint8_t *buf, size_t len)
{
    uint8_t a[2] = {(uint8_t)(mem >> 8), (uint8_t)mem};
    return i2c_master_transmit_receive(s_dev, a, sizeof(a), buf, len, XFER_TIMEOUT_MS);
}

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

    /* 4 KB once at boot, into a static buffer rather than the stack. */
    static uint8_t image[PR_SLOTS * PR_SLOT_SIZE];
    for (size_t off = 0; off < sizeof(image); off += READ_CHUNK) {
        err = eeprom_read((uint16_t)off, &image[off], READ_CHUNK);
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

    uint8_t buf[2 + PR_SLOT_SIZE];
    uint16_t mem = (uint16_t)(slot * PR_SLOT_SIZE);
    buf[0] = (uint8_t)(mem >> 8);
    buf[1] = (uint8_t)mem;
    pr_encode(&buf[2], s, next.seq);

    esp_err_t err = i2c_master_transmit(s_dev, buf, sizeof(buf), XFER_TIMEOUT_MS);
    if (err == ESP_OK) {
        err = wait_write_cycle();
    }
    /* Advance only on success: a failed write leaves the cursor where it
     * was, so the next attempt retries the same slot. */
    if (err == ESP_OK) {
        s_cur = next;
    }
    return err;
}
