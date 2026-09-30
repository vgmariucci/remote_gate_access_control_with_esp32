# 0005 — Guest codes live on the EEPROM, five of them, slot for slot

Status: accepted
Date: 2026-09-28

## Context

ADR 0001 says the gate validates offline: a code is checked against a
table in RAM, with no network in the path. That table was populated only
by `tg_client` pushes, and lived only in RAM.

So a power cut erased every guest code. The gate came back up, refused
the code a guest was holding, and stayed shut until the backend happened
to push again. An outage that takes down the router and the ESP32
together — the common case, since they share a building — produces
exactly this: a guest at the gate with a valid code the lock has
forgotten. The offline-validation promise was only as good as the last
reboot.

The DS3231M has no SRAM (ADR 0004, amended). The ZS-042 board carries an
AT24C32 EEPROM on the same bus, 4 KB, needing no battery.

## Decision

Guest codes are written to the EEPROM and restored at boot.

### Five slots, in RAM and on the chip alike

`AC_MAX_SLOTS` is 5, and the code region holds 5 records. One capacity,
not two.

The alternative was 16 RAM slots over 5 persistent ones, which raises
"which five get persisted?" and admits a state where a code is accepted
into the table but silently not stored. Matching the counts removes the
question. A dev code occupies a slot like any other; when the table is
full, the console says so.

Five is enough for one door: overlapping check-in and check-out, a
cleaner, the owner.

### The map

| range | bytes | use |
|---|---|---|
| `0x000–0x7FF` | 2048 | attempt-counter ring, 64 slots × 32 B |
| `0x800–0xA7F` | 640 | 5 code slots × 128 B |
| `0xA80–0xFFF` | 1408 | spare |

A code record is 64 bytes — two EEPROM pages — so a write can be cut in
half between them. Each slot therefore holds **two** 64-byte buffers,
written alternately with a rising generation number. Load takes the
newer valid one. An interrupted write leaves the previous code intact:
the same guarantee the attempt ring has, and the reason a power cut
during a `tg_client` push cannot destroy a working code.

Revoking writes an erased record rather than clearing an "occupied" bit,
so a removed code leaves no hash behind.

### Save and load are exact inverses

`persist_codes_save(ctx, i)` writes RAM slot *i* to EEPROM slot *i*.
`persist_codes_load` restores EEPROM slot *i* into RAM slot *i*, through
`pc_apply`.

This is stated as a decision because violating it cost a debugging
session. The loader originally restored through `ac_upsert`, which picks
any free slot and replaces by id. A record living in EEPROM slot 1 was
loaded into RAM slot 0, so `code del` erased slot 0 — already empty —
and reported success, while the real record returned on every boot. Two
records that shared an id collapsed into one and a code vanished.

Both halves of a persistence pair must agree on where things go. When
they disagree, every symptom points somewhere else.

## Consequences

**The hashes sit on an external chip.** Anyone with a $2 I²C adapter can
read the code region. The salt lives in the ESP32's flash, and both are
needed to recover a code from its hash — but an attacker holding the
board can open the door with a screwdriver, so this gives up nothing
that mattered. Recorded rather than mitigated.

**Wear is not a concern here.** Guest codes change a few times a week;
the endurance budget is a million writes per page. The attempt counter,
which an attacker can drive, is the part that needed the wear-levelling
ring (ADR 0004).

**Dev codes are never written.** `persist_codes_save` returns early on a
transient slot, so "a maintenance code cannot outlive the maintenance"
is structural rather than a rule someone has to follow.

## Still open: resync

None of this survives losing the chip another way — corruption, a
replaced board, a forced erase. `tg_client` needs a path for the device
to ask the backend for the current code set, rather than depending on
every code having been pushed exactly once. Until that exists, any loss
of the code region needs manual intervention per guest.
