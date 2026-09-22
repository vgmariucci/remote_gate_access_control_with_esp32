/*
 * ssd1306 - I2C transport for a 128x64 SSD1306 OLED.
 *
 * Shares the bus with the DS3232 (ADR 0004). Everything that decides
 * what the pixels are lives in oled_fb and oled_screens; this file only
 * moves bytes.
 *
 * ADDRESSING: the board silkscreen reads 0x78 / 0x7A. Those are 8-bit
 * write addresses. ESP-IDF wants the 7-bit form, 0x3C / 0x3D. Passing
 * 0x78 here makes the driver address 0xF0 and the display never acks.
 *
 * Page addressing is used rather than horizontal addressing because the
 * SH1106 - frequently sold as an SSD1306 - supports only page mode. One
 * driver therefore handles both; the only difference is col_offset.
 */
#ifndef SSD1306_H
#define SSD1306_H

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "oled_fb.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t address;    /* 7-bit */
    uint8_t col_offset; /* 0 for SSD1306, 2 for SH1106 */
    uint32_t scl_hz;    /* 400 kHz is fine on a bench; drop it for long runs */
} ssd1306_config_t;

#define SSD1306_DEFAULT_CONFIG()                                                              \
    {                                                                                         \
        .address = 0x3C, .col_offset = 0, .scl_hz = 400000,                                   \
    }

esp_err_t ssd1306_init(i2c_master_bus_handle_t bus, const ssd1306_config_t *cfg);

/* Sends only the pages that changed since the last successful flush.
 * During entry the countdown bar touches one or two pages, so a tick
 * costs ~130-260 bytes rather than 1032; while idle it costs nothing. */
esp_err_t ssd1306_flush(const oled_fb_t *fb);

/* Forces a full resend on the next flush. Use after a bus error, since
 * the panel's RAM may no longer match the shadow copy. */
void ssd1306_invalidate(void);

/* Panel on/off. OLED pixels burn in under static content; blank the
 * panel after inactivity and wake it on a keypress. */
esp_err_t ssd1306_power(bool on);

esp_err_t ssd1306_contrast(uint8_t level);

#ifdef __cplusplus
}
#endif

#endif /* SSD1306_H */
