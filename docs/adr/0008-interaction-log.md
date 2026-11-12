# 0008 — The gate keeps an interaction log and pushes it outward

Status: accepted
Date: 2026-11-08

## Context

Everything the gate knows about what happened at the door is in the
serial log, which nobody reads, and in a failure counter that survives
reboot only as a number. When a guest says "it did not open", there is
no record to consult. When something opens the gate at four in the
morning, there is no record of that either.

ADR 0007 made the device a puller: it asks for the code set and
installs it. Nothing has ever travelled the other way. A log changes
that, and it is the first time the device will write to the backend —
so the direction, the authentication and the failure modes all need
settling before any of it is built.

The constraint that shapes the rest: **the device must not know who
the guest is.** It is a box bolted to a wall outside, and it can be
unscrewed and carried away. It already holds no plaintext codes
(ADR 0005) and no guest identity; a log that put names on it would
undo that for the sake of a convenience the backend can provide
anyway.

## Decision

**The gate records fixed-size events in EEPROM and uploads them to the
backend, signed, at least once, keeping each until the backend
acknowledges it by sequence number.**

The device logs `code_id`, not a guest. The backend already knows which
code belongs to which stay, so the name is a join at display time.

### Where the events live

The spare region of the AT24C32, `0xA80`–`0xFFF`: 1408 bytes. At
16 bytes per record that is **88 events**, and two records fit exactly
in the 32-byte page the chip writes, so no record ever straddles a page
boundary.

```
  offset  size  field
       0     4  seq          monotonic per gate, never reused
       4     4  timestamp    unix seconds, UTC
       8     1  event        see below
       9     1  result       ac_result_t, for a denial
      10     4  code_id      the 8 hex characters, packed as 32 bits
      14     1  flags        bit 0: the clock was trusted
      15     1  crc8         slot validity
```

Records are self-describing and the ring is scanned at boot to find its
head, exactly as `persist_ring` already does for the attempt counter.
That is deliberate reuse: it means a torn write leaves a slot that
fails its CRC rather than a cursor that disagrees with the data.

Only `last_acked_seq` lives in NVS. It is the one piece of state that
cannot be recovered by looking at the ring.

The CRC is weak on purpose. Its job is to tell a written slot from an
empty or half-written one; integrity in transit is the MAC's job, and
duplicating that work in a byte would buy nothing.

**Event types:** boot (carrying the reset reason), granted, denied,
lockout started, maintenance session opened, maintenance session
closed. A denial carries its `ac_result_t`, so "wrong code" and
"outside its window" and "the clock was not trusted" stay
distinguishable — they mean very different things to whoever is
reading.

At a handful of events a day, 88 slots is roughly two weeks of outage
before anything is lost.

### The wire format

Text, one record per line, same reasoning as `sync_parse`: no parser
dependency, one obvious byte range to sign, readable by a person at
three in the morning.

```
portao-log v1
gate <device-id>
dropped <n>
event <seq> <ts> <type> <result> <id|-> <flags>
event ...
end
mac <hex>
```

The MAC covers everything up to and including `end\n`, with the
device's delivery key.

**The first line is the domain separator.** The same key signs
`portao-sync v1` bodies coming down and `portao-log v1` batches going
up. Because the magic differs in the signed region, no message of one
kind can ever be presented as the other, whatever a future format
change does to the fields underneath. This costs nothing and closes the
question permanently.

### The acknowledgement must be signed too

The backend replies with the highest sequence number it has durably
stored, and the device only then forgets those events:

```
portao-ack v1
gate <device-id>
acked <seq>
end
mac <hex>
```

Signed, because from the device's side an ack is a **delete
primitive**. An unauthenticated one lets anyone who can answer the
device tell it to discard the record of what just happened at the door
— which is precisely what someone who just walked through it would
want. TLS already prevents that for an on-path attacker; the MAC is
what makes it untrue regardless of who the device ended up talking to.

### At least once, deduplicated at the far end

The device re-sends until acked. The backend's primary key is
`(gate_id, seq)`, so a duplicate is free. A crash between storing and
acking costs a repeat, never a loss.

### Holes are reported, not hidden

When the ring wraps before an upload succeeds, the oldest events are
overwritten and a counter is incremented. That counter rides on the
next batch as `dropped <n>`.

Sequence numbers are monotonic and never reused, so a gap is also
visible by inspection at the backend. Both exist because they answer
different questions: a gap in the sequence says *something is missing*,
and the `dropped` counter says *it is missing permanently, stop waiting
for it*.

A log with a silent hole is worse than one that admits it lost eleven
events.

### Two timestamps, never one

Every row stores the device's clock **and** the server's arrival time,
plus the `clock_trusted` flag the firmware already tracks.

A gate that boots before SNTP has a plausible-looking wrong clock. A
log that silently mixes trusted and untrusted times is worse than one
that says which is which, because the first can only be discovered by
noticing that an event happened in 1970.

### The log does not reference the code table

`code_id` is stored as plain characters, with no foreign key.

An audit record has to survive the deletion of the thing it describes.
Constraining the log to the code table would mean that cleaning up
codes either fails or takes the history with it, and both are worse
than an occasional id that no longer joins to anything.

### Append-only

`INSERT` and `SELECT` for the application role; no `UPDATE`, no
`DELETE`. A log that the application can edit is not a log.

Retention deletion is a separate, privileged job that removes rows by
age and nothing else. That is the only thing permitted to remove a row,
and it is not something the API can be talked into doing.

## Consequences

**The backend gains a public, writing endpoint.** Until now everything
the device touched was a read. An ingest path that accepts a POST and
writes rows is a different kind of exposure: the MAC must be verified
before anything reaches the database, the batch size must be bounded,
and it wants a rate limit. Verifying first and parsing second is the
same order `tg_proto` already uses, for the same reason.

**It does not matter if it fails.** Uploading is not on the path to
opening a gate. A backend that is down costs retained events and
nothing else, until the ring wraps — and then it costs the oldest of
them, visibly. The gate opens for a valid code with no network at all
(ADR 0001), and that stays true.

**The log is pseudonymous by construction, which matters legally.**
An access record tied to an identifiable guest is personal data, and
this is operating in Brazil under the LGPD. Because the device only
ever knows `code_id`, identity lives in exactly one place — the stay
record in Postgres — so "forget this guest" is one deletion, after
which the log survives as opaque ids that join to nothing. Retention
should be a period chosen on purpose rather than "forever by default".
This is a design consequence and not legal advice; if the rental grows
beyond a private arrangement it deserves a real opinion.

**Wrong codes are unattributable, permanently.** A denial against an
unknown hash has no id, because there is nothing to attribute it to.
"Someone typed three wrong codes at 01:45" is the most the system can
ever say. That is correct behaviour rather than a gap to be closed
later, and the admin UI should not imply otherwise.

**The timestamp overflows in 2106.** Four bytes of unix seconds, chosen
to keep the record at 16 and two to a page. Noted rather than solved.

## Status visibility

The amber "sync stale" indicator (ADR 0007) covers the pull direction
only. A gate whose uploads have been failing for a week looks entirely
healthy from the outside, which is the failure this project keeps
rediscovering.

- `log` on the dev console prints the ring: head, how many are
  unacked, the dropped counter, and the oldest retained sequence
- the backend knows the last batch it accepted per gate, which is the
  one place where "this gate has gone quiet" can actually be noticed
- nothing on the panel. A guest at the door should not be told about
  the backend, and the LED already carries the states that affect
  whether the gate will open
