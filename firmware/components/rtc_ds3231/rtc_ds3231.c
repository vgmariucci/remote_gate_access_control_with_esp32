#include "rtc_ds3231.h"

#include "esp_log.h"
#include <string.h>
#include <time.h>

static const char *TAG = "rtc";

#define REG_SECONDS 0x00
#define REG_STATUS 0x0F
#define STATUS_OSF 0x80

static i2c_master_dev_handle_t s_dev;
static bool s_trusted;

static uint8_t bcd_to_bin(uint8_t v)
{
    return (uint8_t)((v >> 4) * 10 + (v & 0x0F));
}
static uint8_t bin_to_bcd(uint8_t v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

/* Days since 1970-01-01 for a civil date, by Howard Hinnant's algorithm.
 * newlib on ESP-IDF does not declare timegm, and mktime is wrong here
 * because it applies the local timezone while the RTC holds UTC. Doing
 * the arithmetic here also keeps the driver independent of libc.
 * Verified against glibc timegm for every date from 2026 to 2050. */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= (m <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 1000);
}

static esp_err_t reg_write(uint8_t reg, const uint8_t *buf, size_t len)
{
    uint8_t tmp[1 + 8];
    if (len > sizeof(tmp) - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    tmp[0] = reg;
    memcpy(&tmp[1], buf, len);
    return i2c_master_transmit(s_dev, tmp, len + 1, 1000);
}

esp_err_t rtc_ds3231_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DS3231_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        return err;
    }
    int64_t epoch;
    s_trusted = (rtc_ds3231_get_time(&epoch) == ESP_OK);
    if (s_trusted) {
        ESP_LOGI(TAG, "RTC holds a plausible time");
    } else {
        ESP_LOGW(TAG, "RTC not trusted: refusing all codes until the time is set");
    }
    return ESP_OK;
}

esp_err_t rtc_ds3231_get_time(int64_t *out_epoch)
{
    if (out_epoch == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t status;
    esp_err_t err = reg_read(REG_STATUS, &status, 1);
    if (err != ESP_OK) {
        return err;
    }
    if (status & STATUS_OSF) {
        ESP_LOGW(TAG, "oscillator stop flag set");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t r[7];
    err = reg_read(REG_SECONDS, r, sizeof(r));
    if (err != ESP_OK) {
        return err;
    }
    unsigned sec = bcd_to_bin(r[0] & 0x7F);
    unsigned min = bcd_to_bin(r[1] & 0x7F);
    unsigned hour = bcd_to_bin(r[2] & 0x3F); /* 24 h, forced on write */
    unsigned day = bcd_to_bin(r[4] & 0x3F);
    unsigned mon = bcd_to_bin(r[5] & 0x1F);
    int year = 2000 + bcd_to_bin(r[6]) + ((r[5] & 0x80) ? 100 : 0);

    /* Garbage BCD would otherwise feed nonsense into the date maths. */
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || hour > 23 || min > 59 || sec > 59) {
        ESP_LOGW(TAG, "nonsense BCD in the RTC registers");
        return ESP_ERR_INVALID_STATE;
    }

    int64_t epoch =
        days_from_civil(year, mon, day) * 86400 + (int64_t)hour * 3600 + min * 60 + sec;
    if (epoch < RTC_MIN_PLAUSIBLE_EPOCH || epoch > RTC_MAX_PLAUSIBLE_EPOCH) {
        ESP_LOGW(TAG, "implausible RTC reading");
        return ESP_ERR_INVALID_STATE;
    }
    *out_epoch = epoch;
    return ESP_OK;
}

esp_err_t rtc_ds3231_set_time(int64_t epoch)
{
    if (epoch < RTC_MIN_PLAUSIBLE_EPOCH || epoch > RTC_MAX_PLAUSIBLE_EPOCH) {
        return ESP_ERR_INVALID_ARG;
    }
    time_t tt = (time_t)epoch;
    struct tm t;
    gmtime_r(&tt, &t);

    uint8_t r[7] = {
        bin_to_bcd((uint8_t)t.tm_sec),
        bin_to_bcd((uint8_t)t.tm_min),
        bin_to_bcd((uint8_t)t.tm_hour), /* bit 6 clear: 24 h */
        (uint8_t)(t.tm_wday + 1),
        bin_to_bcd((uint8_t)t.tm_mday),
        (uint8_t)(bin_to_bcd((uint8_t)(t.tm_mon + 1)) | (t.tm_year >= 200 ? 0x80 : 0)),
        bin_to_bcd((uint8_t)(t.tm_year % 100)),
    };
    esp_err_t err = reg_write(REG_SECONDS, r, sizeof(r));
    if (err != ESP_OK) {
        return err;
    }

    uint8_t status;
    err = reg_read(REG_STATUS, &status, 1);
    if (err == ESP_OK) {
        status &= (uint8_t)~STATUS_OSF;
        err = reg_write(REG_STATUS, &status, 1);
    }
    if (err == ESP_OK) {
        s_trusted = true;
    }
    return err;
}

bool rtc_ds3231_clock_trusted(void)
{
    return s_trusted;
}