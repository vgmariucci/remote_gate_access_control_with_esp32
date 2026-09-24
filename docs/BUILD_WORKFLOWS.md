# Build workflows

Two kinds of build, two kinds of terminal, and they must never be mixed.

| | target build | host tests |
|---|---|---|
| directory | `firmware/` | `firmware/host_test/` |
| target | `esp32s3` | `linux` |
| compiler | `xtensa-esp32s3-elf-gcc` | `/usr/bin/cc` |
| terminal | fresh, untouched | after running `idf-host` |
| prompt | normal | `(host)` |

The two are incompatible in one shell: `idf-host` removes the Xtensa
toolchain from `PATH`, and it cannot be undone. Open a new terminal to
switch back.

---

## One-time setup

Do these once; they are what stops the problem recurring.

### 1. `idf-host` in `~/.bashrc`

```bash
idf-host() {
  if [ -n "$IDF_HOST_MODE" ]; then
    echo "already in host mode"
    return 0
  fi
  export IDF_HOST_MODE=1
  export PS1="(host) $PS1"
  unset IDF_TARGET
  export PATH=$(echo "$PATH" | tr ':' '\n' \
    | grep -v '.espressif/tools/xtensa-esp-elf' \
    | grep -v '.espressif/tools/riscv32-esp-elf' \
    | grep -v '.espressif/tools/esp32ulp-elf' \
    | paste -sd:)
  echo "host mode: as=$(which as). Target builds need a NEW terminal."
}
```

`unset IDF_TARGET` matters: with it set, `set-target linux` refuses.
The `(host)` prompt is what tells you which terminal you are in.

### 2. Each project pins its own target

ESP-IDF reads the target from `sdkconfig.defaults` when the environment
does not override it. Both files must exist:

```bash
cd firmware
grep CONFIG_IDF_TARGET sdkconfig.defaults            # esp32s3
echo 'CONFIG_IDF_TARGET="linux"' > host_test/sdkconfig.defaults
grep CONFIG_IDF_TARGET host_test/sdkconfig.defaults  # linux
```

Commit both. A fresh clone then builds correctly with no manual
`set-target` anywhere.

### 3. Install the host prerequisites

```bash
sudo apt install -y libbsd-dev clang-format
```

`libbsd-dev` is needed only by the linux target; the container in CI
already has it, which is why CI never complained.

---

## Target build: flash and monitor

In a **fresh** terminal (no `(host)` in the prompt):

```bash
cd firmware
idf.py -p /dev/ttyACM0 flash monitor
```

`Ctrl+]` exits the monitor.

If the USB device is missing after a reboot or replug, re-attach it from
an **Administrator PowerShell** on Windows:

```powershell
usbipd attach --wsl --busid 1-4
```

## Host tests

In a terminal you are willing to give up for target builds:

```bash
idf-host                 # prompt becomes (host)
cd firmware/host_test
idf.py build && ./build/access_core_host_test.elf
```

Expect `85 Tests 0 Failures`.

---

## When the target is wrong

`sdkconfig` is generated and is not in git. When it holds the wrong
target, nothing else will work until it is regenerated:

```bash
rm -rf build sdkconfig
idf.py set-target esp32s3        # in firmware/
idf.py --preview set-target linux # in host_test/, from a (host) terminal
```

**Read the output of `set-target` before building.** It must say
`Building ESP-IDF components for target <what you asked for>`. If it
does not, building on top just produces a confusing error somewhere
else.

### One tell that settles it instantly

If the build output mentions a **bootloader**, a **partition table**, or
an **esptool** command, it is building for a chip. A genuine linux build
produces none of those.

---

## Diagnosis

| Symptom | Cause | Fix |
|---|---|---|
| `as: unrecognized option '--64'` | Target terminal, host build. The Xtensa assembler is shadowing `/usr/bin/as` | `idf-host`, or a terminal that already has it |
| `cannot execute binary file: Exec format error` | `host_test` built for a chip | `rm -rf build sdkconfig; idf.py --preview set-target linux` |
| `xtensa-...-gcc is not a full path and was not found` **in a `(host)` terminal** | Wrong target, not a broken toolchain. The linux target uses `/usr/bin/cc` | regenerate the target as above |
| Same error in a **normal** terminal | Toolchain genuinely missing from `PATH` | open a fresh terminal; check `which xtensa-esp32s3-elf-gcc` |
| `Target 'x' is not consistent with target 'y' in the environment` | `IDF_TARGET` is exported | `unset IDF_TARGET`, then `set-target` |
| `bsd/sys/cdefs.h: No such file` | `libbsd-dev` missing | `sudo apt install libbsd-dev` |
| `#error "Wrong target. This firmware is ESP32-S3 only."` | The guard in `app_main.c` doing its job | follow the instruction in the message |
| `addr2line: No such file or directory` in the monitor | Monitoring from a `(host)` terminal; backtraces cannot be decoded | flash and monitor from a normal terminal |

---

## Before any build, three questions

1. Which directory am I in — `firmware/` or `host_test/`?
2. Does the prompt say `(host)`, and is that what this build needs?
3. Does `sdkconfig` hold the target I want?

```bash
pwd; echo "$PS1" | grep -q host && echo "HOST terminal" || echo "target terminal"
grep CONFIG_IDF_TARGET= sdkconfig 2>/dev/null || echo "no sdkconfig yet"
```
