#ifndef DEV_CONSOLE_H
#define DEV_CONSOLE_H

/* Starts the `gate>` REPL on USB-Serial/JTAG. No-op unless
 * CONFIG_GATE_DEV_CONSOLE is set. */
void dev_console_start(void);

#endif /* DEV_CONSOLE_H */
