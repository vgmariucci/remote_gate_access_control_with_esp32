# 0004 — One attempt counter, owned by access_core, persisted in EEPROM

Status: accepted (amended 2026-09-22)
Date: 2026-09-19

## Context

Two counters existed for one concept.

`access_core.failed_attempts` drives the decision: it increments on a
denial and arms `lockout_until` once it reaches `max_failed_attempts`.
`ui.attempts_used` drove the display.

They had different reset semantics. `register_failure()` zeroed
`failed_attempts` at the moment it armed the lockout, so the UI could not
read it to show "5 of 5", which is why the second counter existed. They
also worked in different units: `ui` in monotonic milliseconds since
boot, `access_core` in epoch seconds.

Both were correct in isolation and passed their own tests. Wiring them
together would have produced two counters disagreeing about the same
guest, the stricter one winning silently through an untested path.

## Decision

One counter, owned by `access_core`. `ui` reads it for display and keeps
none of its own.

- `register_failure()` no longer zeroes the counter when it arms the
  lockout. `ac_tick()` clears both once the deadline passes.
- `ac_is_locked_out()` is a const, side-effect-free query, so the
  renderer can call it every frame.
- The lockout screen is derived from `ac_ctx_t`, not stored in `ui`.
- Epoch seconds are the single unit for the lockout deadline.

## Persistence

`failed_attempts` and `lockout_until` survive a reboot, restored through
`ac_restore_attempts()` before the UI starts.

**A deadline, not a remaining duration.** `lockout_until` is an absolute
epoch, and the RTC knows what time it is. Restoring "locked until
14:32:07" after a reboot is exact; restoring "3800 seconds remaining"
would need to know how long the power was out.

This composes with ADR 0002: if the oscillator-stop flag is set after a
full power loss, time is untrusted and every code is refused anyway, so
a restored deadline is moot in exactly the case where it could not be
interpreted.

### Amendment, 2026-09-22: EEPROM, not RTC SRAM

The original decision stored the counter in the DS3232's 236 bytes of
battery-backed SRAM. The part fitted to the bench board is a
**DS3231M**, which has no SRAM: its register map ends at 0x12. Earlier
firmware wrote the counter to 0x14 on every boot, an address that does
not exist on this chip.

The counter moves to the **AT24C32** EEPROM on the same ZS-042 board and
bus, at 0x57. That is arguably better: it needs no battery at all.

EEPROM has finite write endurance, about one million writes per page,
and every wrong guess is a write. A single fixed location could be worn
out by a sustained brute-force attempt within a couple of years. The
store is therefore a **wear-levelling ring** (`persist_ring`):

- 128 slots of 32 bytes, one EEPROM page each, so a slot write never
  straddles a page boundary.
- Each save goes to the slot after the newest, with a sequence number one
  higher; load picks the valid record with the highest sequence number.
  Endurance rises 128-fold, well beyond the life of the hardware.
- Each record carries a CRC. A write interrupted by a power cut fails the
  check and is skipped, so load falls back to the previous record. A torn
  write can never yield a corrupted counter.

All three properties are tested on the host by simulating the EEPROM as
a byte image and corrupting it mid-write.

### Known limitation

A torn write rolls the counter back by one. An attacker able to cut
power within the few milliseconds of the write after each wrong guess
could, in principle, keep the counter from climbing. The battery backup
makes that hard to do from outside the enclosure. The complete fix is
charge-first accounting: persist the incremented counter *before*
evaluating the code, and refund it on a success. Deferred, recorded here.
