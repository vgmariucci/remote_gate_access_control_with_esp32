# portao-acesso

Remote gate access control for short-term rentals. An ESP32-S3 at the gate
validates a keypad code entirely offline; a web app generates the code and
pushes only its hash to the device through a Telegram bot. The plaintext
reaches the guest through the Airbnb chat, sent by the host.

## Why the code is never sent to the device

The device stores `sha256(code || device_salt)` and nothing else. Telegram
carries the hash, not the code. A dumped flash image, a leaked bot token or
a compromised Telegram account therefore yields nothing that opens the gate.

## Layout

```
firmware/                ESP-IDF project (target: esp32s3)
  main/                  app entry point, wires the modules together
  components/
    access_core/         pure C99 decision logic, zero ESP dependencies
      test/              Unity tests, shared by host and target runs
  host_test/             linux-target app that runs those tests in CI
  partitions.csv         dual OTA slots + encrypted NVS
  sdkconfig.defaults     the reproducible part of the config (committed)
webapp/                  FastAPI + Next.js admin SPA (milestone W1+)
docs/adr/                architecture decision records
.github/workflows/       CI
```

`access_core` has no ESP-IDF dependency on purpose. That is what lets the
whole decision path be tested on a GitHub runner with no hardware
attached, which is where solo embedded projects usually give up on CI.

## Requirements

- ESP-IDF v5.4 (match the version pinned in `.github/workflows/firmware-ci.yml`)
- VSCode with the Espressif IDF extension
- An ESP32-S3 DevKit, a 4x4 matrix keypad, a DS3231 RTC, a relay module

## Getting started

```bash
git clone <your-repo> && cd portao-acesso

# Run the unit tests on your machine, no board needed
cd firmware/host_test
idf.py --preview set-target linux
idf.py build && ./build/access_core_host_test.elf

# Build and flash the firmware
cd ../
idf.py set-target esp32s3
idf.py build flash monitor

# Run the same tests on the device
idf.py -T access_core build flash monitor
```

## Branching and releases

Trunk-based. `main` is always green and always flashable. Work happens on
short-lived branches named `feat/keypad-scan`, `fix/rtc-drift`,
`chore/ci-cache`, merged by PR with CI required. Conventional Commits, so
the changelog can be generated rather than written.

Firmware releases are semver tags, `fw-v0.2.0`. Tagging is what publishes
an OTA image, so never tag a commit that CI has not built.

## Security notes for contributors

- Never commit `.env`, a bot token, an HMAC key or a device salt.
- Never log a typed code, not even at DEBUG. Log at most 8 hex characters
  of its hash.
- Release builds go out at log level WARN so a forgotten serial console
  cannot leak ids.

## Status

| Milestone | State |
|---|---|
| 0 - repo, toolchain, CI | done |
| 1 - access_core (format, slots, expiry, lockout) | done, 17 tests |
| 2 - keypad driver | next |
| 3 - NTP + DS3231 trusted clock | |
| 4 - encrypted NVS persistence | |
| 5 - relay pulse and feedback | |
| 6 - Wi-Fi manager | |
| 7 - Telegram long-poll client | |
| 8 - attempt log upload | |
| 9 - OTA, watchdog, factory reset | |
