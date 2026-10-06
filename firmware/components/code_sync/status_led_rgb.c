#include "status_led_rgb.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "led";

static led_strip_handle_t s_strip;
static led_colour_t s_colour = LED_GREEN;
static bool s_lit;
static bool s_valid; /* something has been written at least once */

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
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 20 * 1000 * 1000,
        .mem_block_symbols = 128,
        .flags =
            {
                .with_dma = false,
            },
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
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
    bool lit = led_is_on(now_ms);

    /* The main loop runs every few milliseconds; the LED changes twice
     * a minute. Writing only on a change keeps RMT off the hot path. */
    if (s_valid && want == s_colour && lit == s_lit) {
        return;
    }
    if (want != s_colour) {
        ESP_LOGI(TAG, "status: %s", led_colour_name(want));
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
    if (s_strip != NULL) {
        show(c);
        s_valid = false; /* let the next tick take it back */
    }
}

led_colour_t status_led_current(void)
{
    return s_colour;
}