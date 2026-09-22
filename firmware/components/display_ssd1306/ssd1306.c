#include "ssd1306.h"

#include "esp_log.h"
#include <string.h>

static const char *TAG = "oled";

#define CTRL_CMD 0x00
#define CTRL_DATA 0x40
#define I2C_TIMEOUT_MS 50

static i2c_master_dev_handle_t s_dev;
static ssd1306_config_t s_cfg;
static oled_fb_t s_shadow;
static bool s_shadow_valid;

static esp_err_t send_cmds(const uint8_t *cmds, size_t n)
{
    uint8_t buf[1 + 32];
    if (n > sizeof(buf) - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = CTRL_CMD;
    memcpy(&buf[1], cmds, n);
    return i2c_master_transmit(s_dev, buf, n + 1, I2C_TIMEOUT_MS);
}

esp_err_t ssd1306_init(i2c_master_bus_handle_t bus, const ssd1306_config_t *cfg)
{
    if (bus == NULL || cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *cfg;

    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = s_cfg.address,
        .scl_speed_hz = s_cfg.scl_hz,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev, &s_dev);
    if (err != ESP_OK) {
        return err;
    }

    /* Standard 128x64 bring-up. Page addressing (0x20, 0x02) is
     * explicit even though it is the reset default, because a warm
     * reset of the ESP32 does not reset the panel. */
    static const uint8_t init_seq[] = {
        0xAE,       /* display off */
        0xD5, 0x80, /* clock divide */
        0xA8, 0x3F, /* multiplex 64 */
        0xD3, 0x00, /* display offset */
        0x40,       /* start line 0 */
        0x8D, 0x14, /* charge pump on */
        0x20, 0x02, /* page addressing */
        0xA1,       /* segment remap: column 127 -> SEG0 */
        0xC8,       /* COM scan descending */
        0xDA, 0x12, /* COM pins, alternative config */
        0x81, 0xCF, /* contrast */
        0xD9, 0xF1, /* precharge */
        0xDB, 0x40, /* VCOMH deselect */
        0xA4,       /* follow RAM */
        0xA6,       /* normal, not inverted */
        0xAF,       /* display on */
    };
    err = send_cmds(init_seq, sizeof(init_seq));
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "no ack at 0x%02X - check wiring, and that the address "
                 "is the 7-bit form (0x3C, not the 0x78 on the silkscreen)",
                 s_cfg.address);
        return err;
    }

    s_shadow_valid = false;
    ESP_LOGI(TAG, "ready at 0x%02X, col offset %u", s_cfg.address, (unsigned)s_cfg.col_offset);
    return ESP_OK;
}

static esp_err_t flush_page(const oled_fb_t *fb, int page)
{
    uint8_t col = s_cfg.col_offset;
    const uint8_t addr[] = {
        (uint8_t)(0xB0 | page),
        (uint8_t)(0x00 | (col & 0x0F)),
        (uint8_t)(0x10 | (col >> 4)),
    };
    esp_err_t err = send_cmds(addr, sizeof(addr));
    if (err != ESP_OK) {
        return err;
    }

    uint8_t buf[1 + OLED_W];
    buf[0] = CTRL_DATA;
    memcpy(&buf[1], &fb->buf[page * OLED_W], OLED_W);
    return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

esp_err_t ssd1306_flush(const oled_fb_t *fb)
{
    if (fb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t dirty = s_shadow_valid ? oled_fb_dirty_pages(&s_shadow, fb) : 0xFF;
    if (dirty == 0) {
        return ESP_OK;
    }

    for (int p = 0; p < OLED_PAGES; p++) {
        if ((dirty >> p) & 1u) {
            esp_err_t err = flush_page(fb, p);
            if (err != ESP_OK) {
                /* The panel may now hold a half-written frame. */
                s_shadow_valid = false;
                return err;
            }
        }
    }
    s_shadow = *fb;
    s_shadow_valid = true;
    return ESP_OK;
}

void ssd1306_invalidate(void)
{
    s_shadow_valid = false;
}

esp_err_t ssd1306_power(bool on)
{
    uint8_t c = on ? 0xAF : 0xAE;
    return send_cmds(&c, 1);
}

esp_err_t ssd1306_contrast(uint8_t level)
{
    const uint8_t c[] = {0x81, level};
    return send_cmds(c, sizeof(c));
}
