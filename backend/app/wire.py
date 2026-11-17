"""Reading a `portao-log v1` batch.

Pure: no database, no network, no I/O. The same reasoning as the
firmware's pure components - this is the part worth testing, so it is
the part with no dependencies.

    portao-log v1
    gate <device-id>
    dropped <n>
    event <seq> <ts> <type> <result> <id|-> <flags>
    event ...
    end
    mac <hex>

The MAC covers everything up to and including `end\\n`. Signing a
terminator rather than stopping at the last event means a truncated
body cannot verify: an attacker who cut the batch after the second
event would otherwise hand over a valid signature over a shorter one.
"""
from __future__ import annotations

from dataclasses import dataclass

MAGIC = "portao-log v1"

# Eight of 16 bytes per event, 88 slots, plus headers: a batch cannot
# legitimately approach this. It is here so that a body which does can
# be refused before anything parses it.
BODY_MAX = 8192

EVENTS = {"boot", "granted", "denied", "lockout", "session_open", "session_close"}

UNUSED = "-"


class WireError(ValueError):
    """The batch is not the right shape. The message never quotes the body."""


@dataclass(frozen=True)
class Event:
    seq: int
    ts: int
    event: str
    result: int | None
    code_id: str | None
    flags: int

    @property
    def clock_trusted(self) -> bool:
        return bool(self.flags & 0x01)

    def as_row(self) -> dict:
        return {
            "seq": self.seq,
            "ts": self.ts,
            "event": self.event,
            "result": self.result,
            "code_id": self.code_id,
            "clock_trusted": self.clock_trusted,
            "flags": self.flags,
        }


@dataclass(frozen=True)
class Batch:
    gate_id: str
    dropped: int
    events: tuple[Event, ...]


def split_signed(body: str) -> tuple[str, str]:
    """Separate the signed region from the MAC, believing neither yet.

    This runs before authentication, so it does the least it can: find
    the boundary, and refuse anything that is not exactly the expected
    shape. No field is interpreted here.
    """
    if not body:
        raise WireError("empty body")
    if len(body) > BODY_MAX:
        raise WireError(f"body over {BODY_MAX} bytes")

    lines = body.split("\n")
    while lines and lines[-1] == "":
        lines.pop()

    if len(lines) < 3:
        raise WireError("too short to be a batch")
    if not lines[-1].startswith("mac "):
        raise WireError("no mac line")
    if lines[-2] != "end":
        raise WireError("no end marker: the body was cut short")

    mac_hex = lines[-1][len("mac "):].strip()
    signed = "\n".join(lines[:-1]) + "\n"
    return signed, mac_hex


def parse(signed: str, expect_gate: str) -> Batch:
    """Interpret an already-authenticated signed region.

    Call this only after the MAC has verified. Parsing first would let
    a forged body steer the parser with its own contents.
    """
    lines = signed.split("\n")
    while lines and lines[-1] == "":
        lines.pop()

    if lines[0] != MAGIC:
        raise WireError("not a portao-log v1 batch")
    if lines[-1] != "end":
        raise WireError("no end marker")

    gate_id: str | None = None
    dropped: int | None = None
    events: list[Event] = []

    for line in lines[1:-1]:
        head, _, rest = line.partition(" ")

        if head == "gate":
            gate_id = rest.strip()
        elif head == "dropped":
            dropped = _int(rest, "dropped", low=0)
        elif head == "event":
            events.append(_event(rest))
        else:
            raise WireError(f"unknown record {head!r}")

    if gate_id is None:
        raise WireError("no gate line")
    if gate_id != expect_gate:
        # A batch addressed elsewhere is refused rather than filed under
        # whichever gate the URL happened to name.
        raise WireError("batch is addressed to another gate")
    if dropped is None:
        raise WireError("no dropped line")

    seqs = [e.seq for e in events]
    if len(set(seqs)) != len(seqs):
        raise WireError("a sequence number appears twice")

    return Batch(gate_id=gate_id, dropped=dropped, events=tuple(events))


def _event(rest: str) -> Event:
    parts = rest.split(" ")
    if len(parts) != 6:
        raise WireError(f"an event has {len(parts)} fields, expected 6")

    seq_s, ts_s, kind, result_s, code_s, flags_s = parts

    if kind not in EVENTS:
        raise WireError(f"unknown event type {kind!r}")

    result: int | None = None
    if result_s != UNUSED:
        result = _int(result_s, "result", low=0, high=255)
    if kind == "denied" and result is None:
        raise WireError("a denial must say why")

    code_id: str | None = None
    if code_s != UNUSED:
        code_id = code_s
        if len(code_id) != 8 or any(c not in "0123456789abcdef" for c in code_id):
            # The device refuses anything else too: a delivered code can
            # never collide with a console-created one.
            raise WireError("not an 8-hex backend id")

    return Event(
        seq=_int(seq_s, "seq", low=0),
        ts=_int(ts_s, "ts", low=0),
        event=kind,
        result=result,
        code_id=code_id,
        flags=_int(flags_s, "flags", low=0, high=255),
    )


def _int(s: str, what: str, low: int, high: int | None = None) -> int:
    s = s.strip()
    # int() accepts "+5", " 5", "5_0" and unicode digits; none of those
    # come off the device, and accepting them here would mean the
    # backend and the firmware disagree about what a number is.
    if not s or not all(c in "0123456789" for c in s):
        raise WireError(f"{what} is not a decimal number")
    v = int(s)
    if v < low or (high is not None and v > high):
        raise WireError(f"{what} out of range")
    return v
