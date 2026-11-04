## Amendment, 2026-11-04: the database commits before the bot speaks

The decision above demoted Telegram to an optimisation and said plainly
that **tg_client is allowed to fail**. That is a statement about the
device, and the device now honours it.

It has a mirror image on the backend that the original decision did not
state, because there was no backend to constrain. There is about to be
one, and the constraint is easier to write down now than to discover
later:

> **A code reaches Telegram only after it is committed to the database
> and the generation has been incremented — both in one transaction.**

Never the other way round, and never in two transactions.

### What the wrong order costs

Push first, commit second. Between the two, a process can die, a write
can be rejected, a deploy can roll back. Suppose it does:

- The gate receives the code over Telegram and installs it. The guest
  arrives, the gate opens. Everything looks correct, because it is.
- The backend's set never contained that code, and the generation never
  moved. So the next poll is answered `unchanged`, and the device keeps
  its table — including a code the backend has never heard of.
- It keeps working. For hours, possibly days. Then something unrelated
  — another guest, a revocation, an admin fixing a typo — increments
  the generation, `sync_apply` replaces every guest code with a set
  that never knew about this one, and the code stops.

The damaging property is not that it fails but *when*: detached in time
from the thing that caused it, at the door, to a guest holding a code
that worked yesterday. Nothing in any log connects the two events.

Commit first and the worst case is dull. If the push then fails, the
code is merely late — at most fifteen minutes, until the next poll
delivers it. That is precisely the failure ADR 0007 already accepts.

### The generation and the set move together, or `unchanged` is a lie

The second half of the rule is the more dangerous one. Three orderings:

1. **Bump, then insert.** A poll landing in the gap receives the new
   generation carrying the old set. The device installs it and persists
   the new number. When the insert commits, the backend is sitting at a
   generation the device already holds, so every later poll is answered
   `unchanged`. **The code is lost permanently.** No retry repairs it,
   because from either side nothing is wrong; only the next unrelated
   bump fixes it, whenever that happens to be.
2. **Insert, then bump.** A poll in the gap is answered `unchanged`
   while the set has in fact changed. The device is briefly stale and
   repairs itself on the next poll. Recoverable — but `unchanged` was
   not true at the moment it was said, and that is a bad habit for a
   protocol whose whole safety argument rests on it.
3. **Both in one transaction.** The only ordering in which the
   generation number actually describes the set it is attached to.

The same care applies to reading. A response assembled by selecting the
generation and then selecting the codes reproduces case 1 under
concurrency even inside a transaction, unless the isolation level
forbids it. The simplest correct form is one query, one snapshot, one
transaction: the generation and the rows that belong to it, together.

### What this makes safe: retrying the push

Once the commit is first, the push can be retried freely. `ac_upsert`
replaces by id, so a code the gate already holds lands on the same slot
and changes nothing — at-least-once delivery is harmless, and the poll
is the truth regardless.

One detail the retry has to respect: tg_proto refuses a counter that is
not greater than the last one accepted, so a retry cannot be a
byte-for-byte resend. It must be re-signed with a fresh counter. The
idempotency lives in the table, not on the wire.

### What still holds

- **tg_client is still allowed to fail.** Unchanged, and this amendment
  is what makes that affordable for the backend as well as the device.
- **The device still does not need to be careful.** It installs what is
  signed and correctly ordered; the poll repairs whatever was missed.
  This amendment constrains the backend and nothing else.
- **No firmware changes.** Nothing here alters `sync_logic`,
  `tg_client` or `access_core`. It is a rule about code not yet
  written, recorded while it is still free.

### Consequences accepted

**The push gets slower by one commit.** A guest waits for a database
round trip before the message leaves. Milliseconds against the fifteen
minutes the bot exists to avoid; not a real cost.

**A crash between commit and push loses nothing.** The code is in the
set, so the next poll delivers it. That is the system working as
designed rather than a case to handle.

**The firmware cannot enforce this.** A committed code and an
uncommitted one are both signed lines with valid MACs, indistinguishable
at the gate. The rule lives in the backend or it lives nowhere — which
is the reason to write it down before the backend exists rather than
after the first guest is turned away.
