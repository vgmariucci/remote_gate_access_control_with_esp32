/*
 * dev_console - bench shell over USB-Serial/JTAG.
 *
 * Exists only so the keypad, display and lock can be exercised on the
 * bench before tg_client can deliver real codes. Compiled to nothing
 * unless CONFIG_GATE_DEV_CONSOLE is set.
 *
 *   gate> time set 1790000000     trust the clock (no NTP yet)
 *   gate> code add 123456AB* 24   valid from now for 24 hours
 *   gate> code clear
 *   gate> status
 *   gate> oled border             bring-up check, 5 s
 *   gate> i2c scan                list responding addresses
 *
 * Note that `code add` puts a plaintext code on the serial line and in
 * the shell history. Acceptable on a bench, and one more reason the
 * console must be off in a deployed unit.
 */
#include "dev_console.h"

#include "sdkconfig.h"

#if !CONFIG_GATE_DEV_CONSOLE

void dev_console_start(void)
{
}

#else

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_console.h"
#include "gate_app.h"
#include "lock_driver.h"
#include "rtc_ds3231.h"

static unsigned s_dev_codes;

static int cmd_time(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "set") == 0) {
        long long epoch = strtoll(argv[2], NULL, 10);
        if (rtc_ds3231_set_time((int64_t)epoch) != ESP_OK) {
            printf("rejected: implausible epoch, or the DS3232 is not answering\n");
            return 1;
        }
        struct timeval tv = {.tv_sec = (time_t)epoch, .tv_usec = 0};
        settimeofday(&tv, NULL);
        gate_app_lock();
        gate_app_set_clock_trusted(true);
        gate_app_unlock();
        printf("clock set to %lld and trusted\n", epoch);
        return 0;
    }
    gate_app_lock();
    bool trusted = gate_app_access()->clock_trusted;
    gate_app_unlock();
    printf("epoch %lld, %s\n", (long long)time(NULL), trusted ? "trusted" : "UNTRUSTED");
    printf("usage: time set <unix-epoch>   (e.g. from `date +%%s` on the PC)\n");
    return 0;
}

static int cmd_code(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "add") == 0) {
        const char *code = argv[2];
        if (!ac_format_valid(code)) {
            printf("invalid: need exactly 9 characters - 6 digits, 2 of A-D, 1 of * or #\n");
            return 1;
        }
        long hours = (argc >= 4) ? strtol(argv[3], NULL, 10) : 24;
        if (hours <= 0 || hours > 24 * 30) {
            hours = 24;
        }

        uint8_t h[AC_HASH_LEN];
        char id[AC_ID_LEN];
        gate_app_lock();
        ac_ctx_t *ac = gate_app_access();
        if (!ac->clock_trusted) {
            gate_app_unlock();
            printf("clock not trusted: run `time set <epoch>` first\n");
            return 1;
        }
        gate_app_hash(code, h);
        snprintf(id, sizeof(id), "dev%05u", ++s_dev_codes);
        int64_t now = (int64_t)time(NULL);
        int slot = ac_upsert(ac, id, h, now - 60, now + (int64_t)hours * 3600);
        gate_app_unlock();
        memset(h, 0, sizeof(h));

        if (slot < 0) {
            printf("table full (%d slots)\n", AC_MAX_SLOTS);
            return 1;
        }
        printf("added %s, valid for %ld h\n", id, hours);
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "clear") == 0) {
        char id[AC_ID_LEN];
        gate_app_lock();
        for (unsigned i = 1; i <= s_dev_codes; i++) {
            snprintf(id, sizeof(id), "dev%05u", i);
            ac_revoke(gate_app_access(), id);
        }
        gate_app_unlock();
        printf("removed %u dev code(s)\n", s_dev_codes);
        s_dev_codes = 0;
        return 0;
    }

    printf("usage: code add <code> [hours] | code clear\n");
    return 1;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    gate_app_lock();
    ac_ctx_t *ac = gate_app_access();
    bool trusted = ac->clock_trusted;
    size_t slots = ac_count(ac);
    unsigned failed = ac->failed_attempts;
    unsigned maxf = ac->max_failed_attempts;
    int64_t lockout = ac->lockout_until;
    gate_app_unlock();

    int64_t now = (int64_t)time(NULL);
    const lock_logic_t *lk = lock_driver_state();

    printf("clock    : %s\n", trusted ? "trusted" : "UNTRUSTED - every code is refused");
    printf("epoch    : %lld\n", (long long)now);
    printf("codes    : %u of %d slots\n", (unsigned)slots, AC_MAX_SLOTS);
    printf("attempts : %u of %u\n", failed, maxf);
    if (lockout > now) {
        printf("lockout  : %lld s remaining\n", (long long)(lockout - now));
    }
    printf("lock     : %u pulse(s), %u refused\n", (unsigned)lk->pulses_total,
           (unsigned)lk->requests_refused);
    return 0;
}

static int cmd_oled(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "border") == 0) {
        gate_app_show_border(5000);
        printf("border for 5 s: all four edges should be crisp\n");
        return 0;
    }
    printf("usage: oled border\n");
    return 1;
}

static int cmd_i2c(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "scan") == 0) {
        i2c_master_bus_handle_t bus = gate_app_bus();
        int found = 0;
        for (uint16_t a = 0x08; a < 0x78; a++) {
            if (i2c_master_probe(bus, a, 20) == ESP_OK) {
                const char *what = a == 0x3C   ? "SSD1306 OLED"
                                   : a == 0x57 ? "AT24C32 EEPROM (attempt counter)"
                                   : a == 0x68 ? "DS3231 RTC"
                                               : "?";
                printf("  0x%02X  %s\n", a, what);
                found++;
            }
        }
        printf("%d device(s)\n", found);
        return 0;
    }
    printf("usage: i2c scan\n");
    return 1;
}

void dev_console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "gate>";
    rc.max_cmdline_length = 128;

    esp_console_dev_usb_serial_jtag_config_t hw =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw, &rc, &repl));

    const esp_console_cmd_t cmds[] = {
        {.command = "time",
         .help = "show or set the clock: time set <epoch>",
         .func = cmd_time},
        {.command = "code", .help = "code add <code> [hours] | code clear", .func = cmd_code},
        {.command = "status", .help = "clock, codes, attempts, lock", .func = cmd_status},
        {.command = "oled", .help = "oled border: bring-up check", .func = cmd_oled},
        {.command = "i2c", .help = "i2c scan: list responding addresses", .func = cmd_i2c},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    esp_console_register_help_command();

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

#endif /* CONFIG_GATE_DEV_CONSOLE */
