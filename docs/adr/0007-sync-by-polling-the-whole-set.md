# 0007 — The device polls for the whole code set; Telegram only makes it fast

Status: accepted
Date: 2026-10-03

## Context

ADR 0005 recorded an open problem: the EEPROM holds the codes, but
nothing restores them when that storage is lost, corrupted, or simply
never received a push. ADR 0006 added a network, and tg_proto added a
delivery format — neither of which closes the gap.

The gap is the delivery model, not the transport. A gate that learns
only from pushed messages is correct exactly when it has received every
message ever sent. Telegram retains updates for a day; a device offline
longer than that misses them permanently, with no way to notice. The
failure is silent and the symptom appears at the door, to a guest, with
a code the backend believes is live.

Pushing harder does not fix this. Any scheme where the device must
reconstruct the current state from a sequence of events needs to know
which events it missed, and a device that has been off cannot know.

## Decision

**The device asks for the complete current code set, on a timer, and
replaces its table with the answer.**

Every 15 minutes while the station is connected, the device sends its
device id and the generation number it currently holds. The backend
replies with a signed body:

- `generation` — monotonic, incremented on every change to this gate's
  code set
- the **complete** set of codes that should be live: id, hash, window
- or, when the generation it was sent is already current, `unchanged`

On a newer generation the device **discards its table and installs what
arrived**. Not a merge, not a patch.

### Why the whole set rather than the new codes

An incremental feed is cheaper and reintroduces the original problem.
To apply only what is new, the device must know what it missed, which
is the state-tracking that failed in the first place. The point of the
poll is that **no history is required to become correct**: one
successful response and the gate is right, whatever happened before it.

Revocation makes this concrete. Removing a code is not a new code; an
incremental feed would need tombstones, retained longer than the
longest outage, applied in order. Sending the whole set makes
revocation the absence of an entry, which needs no retention and no
ordering.

The size argument that usually favours deltas does not apply here. The
table is five slots: an id, a 64-character hash and two timestamps
each, under 600 bytes signed. There is nothing to save.

### Telegram stays, demoted

The bot remains, but it is now an optimisation: it makes a new code
arrive in seconds instead of up to fifteen minutes. A lost push costs
latency and nothing else, because the next poll repairs it. tg_proto's
counter still prevents replay within that channel.

This is worth stating plainly because it changes how the bot should be
judged: **tg_client is allowed to fail.** The poll is the correctness
mechanism.

### The unchanged short-circuit

Sending the held generation lets the backend answer "unchanged" in a
few bytes, which is what nearly every poll will be. The device still
receives the whole truth whenever anything has actually changed, so the
self-correcting property survives the optimisation.

### Signing, and the replay it prevents

Same construction as tg_proto: HMAC-SHA256 over the body, with the
generation inside the signed region. The device refuses any response
whose generation is not strictly greater than the one it holds, and
persists that number in NVS.

Without this, a captured response is a valid response forever: anyone
able to answer the device could restore a revoked code by replaying
yesterday's set. The signature alone does not prevent that; the
generation does.

## Consequences

**An empty set is honoured.** If the backend signs a set with no codes,
the gate ends up with no codes. That is how "revoke everything" has to
work, and it means a backend bug that returns an empty result where it
meant to return an error will lock out every guest until the next
correct poll. We accept this and log it loudly, because the alternative
— refusing to apply empty sets — makes mass revocation impossible, and
mass revocation is the feature you want on the day you need it.

**Up to 15 minutes of staleness is normal.** A revocation that must
take effect immediately needs the push to arrive, or someone at the
gate. Worth remembering when deciding what the admin UI promises.

**The device must tolerate a set larger than its table.** Five slots
(ADR 0005); the backend could send six. The device fills what it can,
in the order received, and logs the overflow rather than failing the
whole sync. A partial table is better than a stale one.

**Dev codes are not touched.** The set from the backend replaces guest
codes only. A console or portal session's transient codes survive a
sync, as they survive everything else, and are revoked by their own
three triggers (ADR 0006).

## Status visibility

A silent sync failure is the thing this ADR exists to prevent, so the
state is made visible without touching the panel policy from the UI
decision (dark until a keypress):

- the RGB LED blinks briefly every 30 s: green online, red offline,
  amber online-but-sync-stale (over an hour), blue portal open, magenta
  clock untrusted — most serious wins
- the OLED shows the detail when something has already woken it
- `net` on the dev console prints the full picture

Amber earns its place: Wi-Fi up but codes stale is the state where
everything looks fine and the gate is quietly wrong.
