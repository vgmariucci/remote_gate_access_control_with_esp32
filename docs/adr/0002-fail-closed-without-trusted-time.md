# ADR 0002: Refuse every code while the clock is untrusted

Status: accepted
Date: 2026-09-13

## Context

Expiry is enforced against the device's own clock. After a power cut with
no internet, an ESP32 without a battery-backed RTC boots at the epoch. An
old code from a guest who checked out three months ago would then appear
to be within its window.

## Decision

`ac_ctx_t.clock_trusted` starts false and is set only after NTP succeeds
or the DS3231 returns a plausible reading. While it is false, `ac_evaluate`
returns `AC_DENIED_NO_CLOCK` for every input, including correct ones.

A DS3231 with its own CR2032 is fitted so that this state is rare: it holds
time for over a year without external power.

## Consequences

A guest can, in principle, face a gate that refuses a valid code. That is
why the mechanical fallback exists: a key lockbox near the gate holding a
spare 433 MHz remote and the motor's manual release key.

We accept an occasional locked-out guest over a permanently valid code.
The first is an inconvenience with a documented workaround; the second is
a stranger with working access to the property indefinitely.
