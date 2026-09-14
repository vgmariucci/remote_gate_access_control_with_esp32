#include "rtc_ds3232.h"

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <time.h>

static const char *TAG = "rtc";

#define DS3232_REG_SECONDS 0x00
#define DS3232_REG_STATUS 0x0F
#define DS3232_STATUS_OSF 0x80

#define PERSIST_MAGIC 0x47415445u /* "GATE" */

static i2c_master_dev_handle_t s_dev;
static bool s_trusted;

/*
 * ESP-IDF 6.0 switched the default C library from Newlib to Picolibc, and
 * timegm() is not part of the guaranteed surface. Ten lines of arithmetic
 * removes the dependency entirely. Howard Hinnant's days_from_civil:
 * days since 1970-01-01 for a proleptic Gregorian date.
 */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= (m <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static uint8_t bcd_to_bin(uint8_t v)
{
    return (uint8_t)((v >> 4) * 10 + (v & 0x0F));
}
static uint8_t bin_to_bcd(uint8_t v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 1000);
}

static esp_err_t reg_write(uint8_t reg, const uint8_t *buf, size_t len)
{
    uint8_t tmp[1 + 16];
    if (len > sizeof(tmp) - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    tmp[0] = reg;
    memcpy(&tmp[1], buf, len);
    return i2c_master_transmit(s_dev, tmp, len + 1, 1000);
}

esp_err_t rtc_ds3232_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DS3232_I2C_ADDR,
        .scl_speed_hz = 100000, /* the DS3232 does 400k, but the OLED
                                 * shares this bus and long jumper runs
                                 * are happier at 100k */
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        return err;
    }

    int64_t epoch;
    if (rtc_ds3232_get_time(&epoch) == ESP_OK) {
        s_trusted = true;
        ESP_LOGI(TAG, "RTC holds a plausible time");
    } else {
        s_trusted = false;
        ESP_LOGW(TAG, "RTC not trusted: refusing all codes until NTP");
    }
    return ESP_OK;
}

esp_err_t rtc_ds3232_get_time(int64_t *out_epoch)
{
    if (!out_epoch) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t status;
    esp_err_t err = reg_read(DS3232_REG_STATUS, &status, 1);
    if (err != ESP_OK) {
        return err;
    }
    if (status & DS3232_STATUS_OSF) {
        ESP_LOGW(TAG, "oscillator stop flag set");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t r[7];
    err = reg_read(DS3232_REG_SECONDS, r, sizeof(r));
    if (err != ESP_OK) {
        return err;
    }

    struct tm tm_val;
    memset(&tm_val, 0, sizeof(tm_val));
    tm_val.tm_sec = bcd_to_bin(r[0] & 0x7F);
    tm_val.tm_min = bcd_to_bin(r[1] & 0x7F);
    tm_val.tm_hour = bcd_to_bin(r[2] & 0x3F); /* forced to 24 h on write */
    tm_val.tm_mday = bcd_to_bin(r[4] & 0x3F);
    tm_val.tm_mon = bcd_to_bin(r[5] & 0x1F) - 1;
    tm_val.tm_year = bcd_to_bin(r[6]) + 100 + ((r[5] & 0x80) ? 100 : 0);

    int64_t epoch = days_from_civil(tm_val.tm_year + 1900, (unsigned)(tm_val.tm_mon + 1),
                                    (unsigned)tm_val.tm_mday) *
                        86400 +
                    (int64_t)tm_val.tm_hour * 3600 + (int64_t)tm_val.tm_min * 60 +
                    (int64_t)tm_val.tm_sec;
    if (epoch < RTC_MIN_PLAUSIBLE_EPOCH || epoch > RTC_MAX_PLAUSIBLE_EPOCH) {
        ESP_LOGW(TAG, "implausible RTC reading");
        return ESP_ERR_INVALID_STATE;
    }

    *out_epoch = epoch;
    return ESP_OK;
}

esp_err_t rtc_ds3232_set_time(int64_t epoch)
{
    if (epoch < RTC_MIN_PLAUSIBLE_EPOCH || epoch > RTC_MAX_PLAUSIBLE_EPOCH) {
        return ESP_ERR_INVALID_ARG;
    }

    time_t t = (time_t)epoch;
    struct tm tm_val;
    gmtime_r(&t, &tm_val);

    uint8_t r[7];
    r[0] = bin_to_bcd((uint8_t)tm_val.tm_sec);
    r[1] = bin_to_bcd((uint8_t)tm_val.tm_min);
    r[2] = bin_to_bcd((uint8_t)tm_val.tm_hour); /* bit 6 clear = 24 h */
    r[3] = (uint8_t)(tm_val.tm_wday + 1);
    r[4] = bin_to_bcd((uint8_t)tm_val.tm_mday);
    r[5] = bin_to_bcd((uint8_t)(tm_val.tm_mon + 1));
    if (tm_val.tm_year >= 200) {
        r[5] |= 0x80; /* century */
    }
    r[6] = bin_to_bcd((uint8_t)(tm_val.tm_year % 100));

    esp_err_t err = reg_write(DS3232_REG_SECONDS, r, sizeof(r));
    if (err != ESP_OK) {
        return err;
    }

    uint8_t status;
    err = reg_read(DS3232_REG_STATUS, &status, 1);
    if (err == ESP_OK) {
        status &= (uint8_t)~DS3232_STATUS_OSF;
        err = reg_write(DS3232_REG_STATUS, &status, 1);
    }
    if (err == ESP_OK) {
        s_trusted = true;
    }
    return err;
}

bool rtc_ds3232_clock_trusted(void)
{
    return s_trusted;
}

esp_err_t rtc_ds3232_sync_ntp_once(uint32_t timeout_ms)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.start = true;
    cfg.sync_cb = NULL;

    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    if (err == ESP_OK) {
        time_t now = 0;
        time(&now);
        err = rtc_ds3232_set_time((int64_t)now);
        ESP_LOGI(TAG, "NTP sync ok, RTC written");
    } else {
        ESP_LOGW(TAG, "NTP sync failed; staying on the RTC");
    }

    esp_netif_sntp_deinit();
    return err;
}

/* --- battery-backed attempt state ------------------------------------ */

typedef struct {
    uint32_t magic;
    uint8_t attempts_used;
    uint8_t reserved;
    uint16_t crc;
    uint32_t lockout_until_epoch;
    uint32_t boot_count;
} __attribute__((packed)) persist_blob_t;

static uint16_t crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

esp_err_t rtc_ds3232_load_persist(rtc_persist_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    persist_blob_t blob;
    esp_err_t err = reg_read(DS3232_SRAM_BASE, (uint8_t *)&blob, sizeof(blob));
    if (err != ESP_OK) {
        return err;
    }

    uint16_t stored = blob.crc;
    blob.crc = 0;
    if (blob.magic != PERSIST_MAGIC || crc16((const uint8_t *)&blob, sizeof(blob)) != stored) {
        /* Fresh cell, or corrupted. Start clean rather than trusting it. */
        memset(out, 0, sizeof(*out));
        return ESP_ERR_NOT_FOUND;
    }

    out->attempts_used = blob.attempts_used;
    out->lockout_until_epoch = blob.lockout_until_epoch;
    out->boot_count = blob.boot_count;
    return ESP_OK;
}

esp_err_t rtc_ds3232_store_persist(const rtc_persist_t *in)
{
    if (!in) {
        return ESP_ERR_INVALID_ARG;
    }
    persist_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.magic = PERSIST_MAGIC;
    blob.attempts_used = in->attempts_used;
    blob.lockout_until_epoch = in->lockout_until_epoch;
    blob.boot_count = in->boot_count;
    blob.crc = 0;
    blob.crc = crc16((const uint8_t *)&blob, sizeof(blob));

    return reg_write(DS3232_SRAM_BASE, (const uint8_t *)&blob, sizeof(blob));
}
