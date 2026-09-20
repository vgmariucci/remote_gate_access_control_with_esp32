# 0004 — One attempt counter, owned by access_core, persisted in RTC SRAM

Status: accepted
Date: 2026-09-19

## Context

Two counters existed for one concept.

`access_core.failed_attempts` drives the decision: it increments on a
denial and arms `lockout_until` once it reaches `max_failed_attempts`.
`ui.attempts_used` drives the display: it feeds "tentativa N de 5" and
the lockout animation.

They had different reset semantics. `register_failure()` zeroed
`failed_attempts` at the moment it armed the lockout, so the UI could
not read it to show "5 of 5" — which is why the second counter existed
in the first place. They also worked in different units: `ui` in
`uint32_t` monotonic milliseconds since boot, `access_core` in `int64_t`
epoch seconds.

Both were correct in isolation, and both passed their own tests. Wiring
them together in `app_main` would have produced two counters disagreeing
about the same guest, with the stricter one silently winning through a
path no test covered.

## Decision

One counter, owned by `access_core`. `ui` reads it for display and keeps
none of its own.

Three changes follow:

- `register_failure()` no longer zeroes `failed_attempts` when it arms
  the lockout. The counter is cleared on a success, or when the lockout
  expires. With that, `access_core.failed_attempts` has exactly the
  semantics `ui.attempts_used` had, and the UI can read it directly.
- `ui_render()` takes the attempt count from the `ac_ctx_t` rather than
  tracking its own.
- Epoch seconds are the single unit for the lockout. `ui` converts for
  its countdown animation at render time and stores nothing.

## Persistence

`failed_attempts` and `lockout_until` are written to the DS3232's 236
bytes of CR2032-backed SRAM, via `ac_restore_attempts()` at boot.

The DS3232 rather than NVS for three reasons: it survives total loss of
system power including the 12 V battery, it costs no flash wear on every
wrong guess, and flash is the one part that cannot be swapped in the
field.

**A deadline, not a remaining duration.** `lockout_until` is an absolute
epoch, and the DS3232 is the component that knows what time it is.
Restoring "locked until 14:32:07" after a reboot is exact. Restoring
"3800 seconds remaining" would require knowing how long the power was
out, which we do not.

This composes correctly with ADR 0002: if the oscillator-stop flag is
set after a full power loss, time is untrusted and every code is refused
anyway, so a restored lockout deadline is moot in exactly the case where
it could not be interpreted.

## Consequences

Without this, the five-attempt lockout is decorative — cut power, reboot
with a clean counter, try five more. The battery makes that harder but
not impossible; there is a fuse and a switch somewhere.

The cost is that `ui` now depends on `access_core` for state as well as
for the verdict. That coupling already existed through `ac_result_t`, so
it adds no new dependency edge, and both remain free of ESP-IDF and
testable on the host.

Superseded in part: ADR 0002 describes the fail-closed clock behaviour
this relies on. The `ui_init(ctx, restored_attempts,
restored_lockout_remaining_ms, now_ms)` signature from step 2 is
replaced by reading the restored state from `ac_ctx_t`.
