/*
 * dev_console - bench shell over USB-Serial/JTAG.
 *
 * Exists so the keypad, display, lock and EEPROM can be exercised
 * before tg_client can deliver real codes. Compiled to nothing unless
 * CONFIG_GATE_DEV_CONSOLE is set.
 *
 *   gate> time set 1790000000      trust the clock (no NTP yet)
 *   gate> code add 123456AB*       dev code: RAM only, 15 min, revoked
 *                                  when the session ends
 *   gate> code persist 123456AB* 24  guest-style code: written to the
 *                                  EEPROM, survives a power cut
 *   gate> code list                what the table holds
 *   gate> code del tst00001        revoke and erase from the EEPROM
 *   gate> code clear               revoke every dev code
 *   gate> dev off                  same, named for the lifecycle
 *   gate> status
 *   gate> oled border              bring-up check, 5 s
 *   gate> i2c scan                 list responding addresses
 *
 * `code add` and `code persist` put a plaintext code on the serial line
 * and in the shell history. Acceptable on a bench, and one more reason
 * the console must be off in a deployed unit.
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
#include "persist_codes.h"
#include "persist_store.h"
#include "rtc_ds3231.h"
#include "status_led_rgb.h"

static unsigned s_dev_codes;

static int cmd_time(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "set") == 0) {
        long long epoch = strtoll(argv[2], NULL, 10);
        if (rtc_ds3231_set_time((int64_t)epoch) != ESP_OK) {
            printf("rejected: implausible epoch, or the DS3231 is not answering\n");
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

/* Shared by `code add` and `code persist`. Returns the slot, or -1. */
static int add_code(const char *code, int64_t seconds, bool persistent, char out_id[AC_ID_LEN])
{
    if (!ac_format_valid(code)) {
        printf("invalid: need exactly 9 characters - 6 digits, 2 of A-D, 1 of * or #\n");
        return -1;
    }

    uint8_t h[AC_HASH_LEN];
    gate_app_lock();
    ac_ctx_t *ac = gate_app_access();
    if (!ac->clock_trusted) {
        gate_app_unlock();
        printf("clock not trusted: run `time set <epoch>` first\n");
        return -1;
    }
    gate_app_hash(code, h);

    int64_t now = (int64_t)time(NULL);
    int slot;
    if (persistent) {
        /* Derived from the clock, not from a counter: a static counter
         * restarts at 1 after every reboot, so two stored codes could
         * end up sharing an id. */
        snprintf(out_id, AC_ID_LEN, "t%07llx", (unsigned long long)(now & 0xFFFFFFF));
        slot = ac_upsert(ac, out_id, h, now - 60, now + seconds);
    } else {
        snprintf(out_id, AC_ID_LEN, "dev%05u", ++s_dev_codes);
        slot = ac_upsert_transient(ac, out_id, h, now - 60, now + seconds);
    }
    gate_app_unlock();
    memset(h, 0, sizeof(h));

    if (slot < 0) {
        printf("table full (%d slots) - `code list` to see what is in it\n", AC_MAX_SLOTS);
    }
    return slot;
}

static int cmd_code(int argc, char **argv)
{
    char id[AC_ID_LEN];

    if (argc >= 3 && strcmp(argv[1], "add") == 0) {
        long minutes = (argc >= 4) ? strtol(argv[3], NULL, 10) : 15;
        if (minutes <= 0 || minutes > 15) {
            minutes = 15;
        }
        int slot = add_code(argv[2], (int64_t)minutes * 60, false, id);
        if (slot < 0) {
            return 1;
        }
        printf("added %s, valid %ld min (dev code: RAM only, cleared when the session ends)\n",
               id, minutes);
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "persist") == 0) {
        long hours = (argc >= 4) ? strtol(argv[3], NULL, 10) : 24;
        if (hours <= 0 || hours > 24 * 30) {
            hours = 24;
        }
        int slot = add_code(argv[2], (int64_t)hours * 3600, true, id);
        if (slot < 0) {
            return 1;
        }

        /* This is the path a real guest code will take. Exercising it
         * here means a power cut proves the EEPROM round trip before
         * anyone is standing at the gate. */
        gate_app_lock();
        esp_err_t err = persist_codes_save(gate_app_access(), slot);
        gate_app_unlock();

        if (err != ESP_OK) {
            printf("added %s in RAM, but the EEPROM write FAILED (0x%x): it will not\n"
                   "survive a reboot. Check `i2c scan` for 0x57.\n",
                   id, (unsigned)err);
            return 1;
        }
        printf("added %s, valid %ld h, written to EEPROM slot %d\n", id, hours, slot);
        printf("power-cycle the board: the boot log should say 1 code(s) restored\n");
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "list") == 0) {
        int64_t now = (int64_t)time(NULL);
        gate_app_lock();
        ac_ctx_t *ac = gate_app_access();
        printf("slot  id        kind     valid\n");
        for (int i = 0; i < AC_MAX_SLOTS; i++) {
            const ac_slot_t *s = &ac->slots[i];
            if (!s->occupied) {
                printf("  %d   -\n", i);
                continue;
            }
            long left = (long)(s->valid_until - now);
            printf("  %d   %-8s  %-7s  ", i, s->id, s->transient ? "dev" : "stored");
            if (left <= 0) {
                printf("expired\n");
            } else if (now < s->valid_from) {
                printf("not yet\n");
            } else {
                printf("%ld min left\n", left / 60);
            }
        }
        gate_app_unlock();
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "del") == 0) {
        gate_app_lock();
        ac_ctx_t *ac = gate_app_access();
        int slot = -1;
        for (int i = 0; i < AC_MAX_SLOTS; i++) {
            if (ac->slots[i].occupied && strncmp(ac->slots[i].id, argv[2], AC_ID_LEN) == 0) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            gate_app_unlock();
            printf("no code with id %s - `code list` to see what is in the table\n", argv[2]);
            return 1;
        }
        bool was_stored = !ac->slots[slot].transient;

        /* A guest's code is not ours to remove on a whim: deleting one
         * locks a paying guest out until the backend re-pushes it. */
        if (!ac_id_is_dev(argv[2]) && !(argc == 4 && strcmp(argv[3], "yes") == 0)) {
            gate_app_unlock();
            printf("%s is a GUEST code from the backend, not a dev code.\n"
                   "Removing it locks that guest out until it is re-pushed.\n"
                   "If you are sure: code del %s yes\n",
                   argv[2], argv[2]);
            return 1;
        }
        ac_revoke(ac, argv[2]);
        /* Write the now-empty slot through, so the record on the chip
         * is erased rather than left behind for the next boot. */
        esp_err_t err = was_stored ? persist_codes_save(ac, slot) : ESP_OK;
        gate_app_unlock();

        if (err != ESP_OK) {
            printf("removed %s from RAM, but the EEPROM erase FAILED (0x%x): it will\n"
                   "come back on the next boot.\n",
                   argv[2], (unsigned)err);
            return 1;
        }
        printf("removed %s%s\n", argv[2], was_stored ? " (and erased from the EEPROM)" : "");
        return 0;
    }

    if (argc == 3 && strcmp(argv[1], "raw") == 0) {
        int slot = atoi(argv[2]);
        if (slot < 0 || slot >= AC_MAX_SLOTS) {
            printf("slot must be 0..%d\n", AC_MAX_SLOTS - 1);
            return 1;
        }
        uint8_t img[PC_SLOT_SIZE];
        esp_err_t err = persist_raw_read(pc_offset(slot, 0), img, sizeof(img));
        if (err != ESP_OK) {
            printf("EEPROM read failed (0x%x)\n", (unsigned)err);
            return 1;
        }
        /* What is actually on the chip, as opposed to what the table
         * says. The two disagreeing is the whole point of this. */
        for (int h = 0; h < 2; h++) {
            const uint8_t *r = &img[h * PC_REC_SIZE];
            pc_code_t c;
            uint32_t gen;
            printf("  half %d @ 0x%03X: ", h, (unsigned)pc_offset(slot, h));
            if (pc_decode(r, &c, &gen)) {
                printf("valid, gen %u, %s%s%s\n", (unsigned)gen,
                       c.occupied ? "occupied, id " : "EMPTY", c.occupied ? c.id : "",
                       c.occupied ? "" : "");
            } else {
                printf("invalid or blank\n");
            }
            printf("       ");
            for (int b = 0; b < 12; b++) {
                printf("%02X ", r[b]);
            }
            printf("\n");
        }
        pc_code_t win;
        uint32_t wgen;
        if (pc_slot_read(img, &win, &wgen)) {
            printf("  winner: gen %u, %s\n", (unsigned)wgen,
                   win.occupied ? win.id : "EMPTY (nothing restores)");
        } else {
            printf("  winner: neither half is valid (nothing restores)\n");
        }
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "erase") == 0) {
        bool confirmed = false;
        bool force = false;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "yes") == 0) {
                confirmed = true;
            } else if (strcmp(argv[i], "all") == 0) {
                force = true;
            }
        }

        gate_app_lock();
        ac_ctx_t *ac = gate_app_access();
        int dev = 0, guest = 0;
        for (int i = 0; i < AC_MAX_SLOTS; i++) {
            const ac_slot_t *sl = &ac->slots[i];
            if (sl->occupied && !sl->transient) {
                if (ac_id_is_dev(sl->id)) {
                    dev++;
                } else {
                    guest++;
                }
            }
        }

        /* Always say what is about to be destroyed, by name. */
        printf("stored codes on the chip:\n");
        for (int i = 0; i < AC_MAX_SLOTS; i++) {
            const ac_slot_t *sl = &ac->slots[i];
            if (sl->occupied && !sl->transient) {
                printf("  slot %d  %-8s  %s\n", i, sl->id,
                       ac_id_is_dev(sl->id) ? "dev" : "GUEST (from the backend)");
            }
        }
        if (dev == 0 && guest == 0) {
            gate_app_unlock();
            printf("  (none)\n");
            return 0;
        }

        if (!confirmed) {
            gate_app_unlock();
            printf("nothing erased. To erase the %d dev code(s): code erase yes\n", dev);
            if (guest > 0) {
                printf("The %d GUEST code(s) are left alone unless you also say: "
                       "code erase all yes\n",
                       guest);
            }
            return 0;
        }

        if (guest > 0 && !force) {
            printf("erasing dev codes only; %d guest code(s) left in place\n", guest);
        }

        int cleared = 0, written = 0, failed = 0;
        for (int i = 0; i < AC_MAX_SLOTS; i++) {
            ac_slot_t *sl = &ac->slots[i];
            bool occupied_stored = sl->occupied && !sl->transient;
            if (occupied_stored && !ac_id_is_dev(sl->id) && !force) {
                continue;
            }
            if (occupied_stored) {
                cleared++;
            }
            memset(sl, 0, sizeof(*sl));
            if (persist_codes_save(ac, i) != ESP_OK || persist_codes_save(ac, i) != ESP_OK) {
                failed++;
            } else {
                written++;
            }
        }
        gate_app_unlock();
        printf("%d code(s) erased, %d slot(s) written%s\n", cleared, written,
               failed ? " (with write failures)" : "");
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "clear") == 0) {
        gate_app_lock();
        int n = ac_revoke_transient(gate_app_access());
        gate_app_unlock();
        s_dev_codes = 0;
        printf("%d dev code(s) revoked (stored codes are untouched)\n", n);
        return 0;
    }

    printf("usage: code add <code> [minutes]     dev code, RAM only, 15 min max\n"
           "       code persist <code> [hours]   stored code, written to the EEPROM\n"
           "       code list\n"
           "       code del <id> [yes]           yes required for a guest code\n"
           "       code clear                    revoke every dev code\n"
           "       code raw <slot>               what is actually on the EEPROM\n"
           "       code erase [all] [yes]        dry run unless yes; all includes\n"
           "                                     guest codes\n");
    return 1;
}

static int cmd_dev(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "off") == 0) {
        gate_app_lock();
        int n = ac_revoke_transient(gate_app_access());
        gate_app_unlock();
        s_dev_codes = 0;
        printf("%d dev code(s) revoked\n", n);
        return 0;
    }
    printf("usage: dev off\n");
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
    int stored = 0, transient = 0;
    for (int i = 0; i < AC_MAX_SLOTS; i++) {
        if (ac->slots[i].occupied) {
            if (ac->slots[i].transient) {
                transient++;
            } else {
                stored++;
            }
        }
    }
    gate_app_unlock();

    int64_t now = (int64_t)time(NULL);
    const lock_logic_t *lk = lock_driver_state();

    printf("clock    : %s\n", trusted ? "trusted" : "UNTRUSTED - every code is refused");
    printf("epoch    : %lld\n", (long long)now);
    printf("codes    : %u of %d slots (%d stored, %d dev)\n", (unsigned)slots, AC_MAX_SLOTS,
           stored, transient);
    printf("attempts : %u of %u\n", failed, maxf);
    if (lockout > now) {
        printf("lockout  : %lld s remaining\n", (long long)(lockout - now));
    }
    printf("lock     : %u pulse(s), %u refused\n", (unsigned)lk->pulses_total,
           (unsigned)lk->requests_refused);
    printf("led      : %s\n", led_colour_name(status_led_current()));
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
                                   : a == 0x57 ? "AT24C32 EEPROM (codes + attempt counter)"
                                   : a == 0x68 ? "DS3231 RTC"
                                               : "? UNEXPECTED - see the note below";
                printf("  0x%02X  %s\n", a, what);
                found++;
            }
        }
        printf("%d device(s)\n", found);
        if (found != 3) {
            printf("Expected exactly 3 (0x3C, 0x57, 0x68). Anything else means the bus is\n"
                   "marginal: fit 4.7k pull-ups from SDA and SCL to 3V3 and scan again.\n");
        }
        return 0;
    }
    printf("usage: i2c scan\n");
    return 1;
}

static int cmd_led(int argc, char **argv)
{
    if (argc < 2) {
        printf("led red|green|blue|amber|magenta\n");
        return 0;
    }
    static const struct {
        const char *n;
        led_colour_t c;
    } map[] = {
        {"red", LED_RED},     {"green", LED_GREEN},     {"blue", LED_BLUE},
        {"amber", LED_AMBER}, {"magenta", LED_MAGENTA},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(argv[1], map[i].n) == 0) {
            status_led_force(map[i].c);
            printf("%s until the next tick\n", led_colour_name(map[i].c));
            return 0;
        }
    }
    printf("unknown colour\n");
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
        {.command = "code",
         .help = "code add | persist | list | del | clear",
         .func = cmd_code},
        {.command = "dev", .help = "dev off: revoke every dev code", .func = cmd_dev},
        {.command = "status", .help = "clock, codes, attempts, lock", .func = cmd_status},
        {.command = "oled", .help = "oled border: bring-up check", .func = cmd_oled},
        {.command = "i2c", .help = "i2c scan: list responding addresses", .func = cmd_i2c},
        {.command = "led", .help = "led red|green|blue|amber|magenta", .func = cmd_led},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    esp_console_register_help_command();

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

#endif /* CONFIG_GATE_DEV_CONSOLE */
