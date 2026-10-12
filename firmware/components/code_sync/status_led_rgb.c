#include "status_led_rgb.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "led";

static led_strip_handle_t s_strip;
static led_colour_t s_colour = LED_GREEN;
static bool s_lit;
static bool s_valid; /* something has been written at least once */

/* A state change is the moment the LED is most worth looking at, so it
 * flashes at once rather than waiting up to thirty seconds for the
 * next window. */
#define CHANGE_FLASH_MS 600u
static uint32_t s_flash_until_ms;
static led_colour_t s_reported = LED_GREEN;
static bool s_reported_valid;

/* `led <colour>` on the console holds the LED long enough to look at.
 * Without this the next tick reclaims it within milliseconds. */
#define FORCE_HOLD_MS 4000u
static uint32_t s_force_until_ms;
static led_colour_t s_force_colour;

/* Visible from across a yard without lighting the place up at night.
 * Raised from the first attempt, which was dim enough that a 40 ms
 * blink could not be told apart from a dead LED. */
#define LVL 60
#define DIM 24

static void rgb_for(led_colour_t c, uint8_t *r, uint8_t *g, uint8_t *b)
{
    switch (c) {
    case LED_GREEN:
        *r = 0;
        *g = LVL;
        *b = 0;
        break;
    case LED_RED:
        *r = LVL;
        *g = 0;
        *b = 0;
        break;
    case LED_AMBER:
        *r = LVL;
        *g = DIM;
        *b = 0;
        break;
    case LED_BLUE:
        *r = 0;
        *g = 0;
        *b = LVL;
        break;
    case LED_MAGENTA:
    default:
        *r = DIM;
        *g = 0;
        *b = LVL;
        break;
    }
}

static void show(led_colour_t c)
{
    uint8_t r, g, b;
    rgb_for(c, &r, &g, &b);
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

esp_err_t status_led_init(gpio_num_t gpio)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = (int)gpio,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        /* led_strip 2.5.x. Version 3 renamed this to
         * color_component_format with LED_STRIP_COLOR_COMPONENT_FMT_GRB. */
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .flags =
            {
                .invert_out = false,
            },
    };
    /* SPI rather than RMT: the whole 24-bit frame goes out by DMA, so
     * a late interrupt cannot stretch a bit. RMT refills its buffer
     * from an interrupt that Wi-Fi can delay, which showed up as
     * colours that were right in the log and wrong on the glass. */
    led_strip_spi_config_t spi_cfg = {
        .clk_src = SPI_CLK_SRC_DEFAULT,
        .spi_bus = SPI2_HOST,
        .flags =
            {
                .with_dma = true,
            },
    };

    esp_err_t err = led_strip_new_spi_device(&strip_cfg, &spi_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WS2812 on GPIO %d failed: %s", (int)gpio, esp_err_to_name(err));
        s_strip = NULL;
        return err;
    }
    led_strip_clear(s_strip);

    /* Walk every colour at boot, 400 ms each, and log the result of
     * each call. Thirty seconds is a long time to wait to learn
     * whether the LED works, and "I saw nothing" is otherwise
     * impossible to tell apart from "nothing was written". If these
     * lines appear and the LED stays dark, the fault is in this driver
     * rather than the wiring. */
    static const led_colour_t sequence[] = {LED_RED, LED_GREEN, LED_BLUE, LED_AMBER,
                                            LED_MAGENTA};
    for (size_t i = 0; i < sizeof(sequence) / sizeof(sequence[0]); i++) {
        uint8_t r, g, b;
        rgb_for(sequence[i], &r, &g, &b);
        esp_err_t set_err = led_strip_set_pixel(s_strip, 0, r, g, b);
        esp_err_t ref_err = led_strip_refresh(s_strip);
        ESP_LOGI(TAG, "self-test %-26s rgb(%3u,%3u,%3u) set=%s refresh=%s",
                 led_colour_name(sequence[i]), (unsigned)r, (unsigned)g, (unsigned)b,
                 esp_err_to_name(set_err), esp_err_to_name(ref_err));
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    led_strip_clear(s_strip);

    ESP_LOGI(TAG, "status LED on GPIO %d: %u ms blink every %u s", (int)gpio,
             (unsigned)LED_BLINK_ON_MS, (unsigned)(LED_BLINK_PERIOD_MS / 1000));
    return ESP_OK;
}

void status_led_tick(const led_inputs_t *in, uint32_t now_ms)
{
    if (s_strip == NULL) {
        return;
    }
    led_colour_t want = led_colour(in);

    if (!s_reported_valid || want != s_reported) {
        ESP_LOGI(TAG, "status: %s", led_colour_name(want));
        s_flash_until_ms = now_ms + CHANGE_FLASH_MS;
        s_reported = want;
        s_reported_valid = true;
    }

    bool lit;
    if ((int32_t)(now_ms - s_force_until_ms) < 0) {
        /* A console request outranks the schedule while it lasts. */
        want = s_force_colour;
        lit = true;
    } else {
        lit = led_is_on(now_ms) || (int32_t)(now_ms - s_flash_until_ms) < 0;
    }

    /* The main loop runs every few milliseconds; the LED changes twice
     * a minute. Writing only on a change keeps the driver off the hot
     * path. */
    if (s_valid && want == s_colour && lit == s_lit) {
        return;
    }

    if (lit) {
        show(want);
    } else {
        led_strip_clear(s_strip);
    }
    s_colour = want;
    s_lit = lit;
    s_valid = true;
}

/* Lights a colour solid until the next tick turns it off again: for
 * `led` on the console, where someone is looking at the board and
 * wants an answer now rather than in thirty seconds. */
void status_led_force(led_colour_t c)
{
    if (s_strip == NULL) {
        return;
    }
    /* Held for a few seconds so there is something to look at; the
     * tick takes the LED back when the hold expires. */
    s_force_colour = c;
    s_force_until_ms = (uint32_t)(esp_timer_get_time() / 1000) + FORCE_HOLD_MS;
    show(c);
    s_colour = c;
    s_lit = true;
    s_valid = true;
}

led_colour_t status_led_current(void)
{
    return s_colour;
}