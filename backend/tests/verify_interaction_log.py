#!/usr/bin/env python3
"""Check the interaction log against ADR 0008.

The properties worth testing are the ones a log is useless without:
that a repeat costs nothing, that nothing can quietly edit it, and
that a hole in the sequence is visible rather than merely absent.
"""
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from pg import banner, check, q, q_fails, summary  # noqa: E402

banner()

GATE = "portao-01"


def events(*rows) -> str:
    """A batch as the API would hand it over, SQL-quoted."""
    out = []
    for seq, ts, ev, result, code_id, trusted in rows:
        out.append({"seq": seq, "ts": ts, "event": ev, "result": result,
                    "code_id": code_id, "clock_trusted": trusted, "flags": 0})
    return json.dumps(out).replace("'", "''")


def ingest(batch: str, dropped: int = 0) -> int:
    return int(q(f"select ingest_log_batch('{GATE}', '{batch}'::jsonb, {dropped});"))


# ---------------------------------------------------------------- setup
q("delete from interaction_upload;")
q("set session gate.retention = 'on'; delete from interaction_log;")
q("delete from gate;")
q(f"insert into gate (gate_id, label) values ('{GATE}', 'bench');")
q(f"""insert into gate_secret (gate_id, device_salt, delivery_key)
      values ('{GATE}', 'bench-salt-not-a-real-one', decode(repeat('9f',32), 'hex'));""")

# Yesterday. Events are things that have already happened, so a fixture
# dated in the future would make received_at < device_time - which is a
# real condition (a gate whose clock is wrong), but not this test's.
T0 = int(time.time()) - 86400

# ------------------------------------------------------- at least once
ack = ingest(events(
    (1, T0 + 10, "boot", 6, None, False),          # task watchdog
    (2, T0 + 20, "granted", 0, "a1b2c3d4", True),
    (3, T0 + 30, "denied", 1, None, True),         # unknown code
))
check("a batch is acked with its highest seq", ack == 3, f"got {ack}")

n = q(f"select count(*) from interaction_log where gate_id='{GATE}';")
check("three events stored", n == "3", f"got {n}")

# The device re-sends until acked, so the same batch must land twice
# without complaint and without duplicating anything.
ack2 = ingest(events(
    (1, T0 + 10, "boot", 6, None, False),
    (2, T0 + 20, "granted", 0, "a1b2c3d4", True),
    (3, T0 + 30, "denied", 1, None, True),
))
n2 = q(f"select count(*) from interaction_log where gate_id='{GATE}';")
check("a replayed batch is idempotent", ack2 == 3 and n2 == "3",
      f"ack {ack2}, rows {n2}")

# A batch that overlaps the last one - the usual shape after a lost ack.
ack3 = ingest(events(
    (3, T0 + 30, "denied", 1, None, True),
    (4, T0 + 40, "lockout", None, None, True),
))
check("an overlapping batch stores only what is new", ack3 == 4, f"got {ack3}")
stored = q("select rows_stored from interaction_upload order by id desc limit 1;")
check("and says so: one new row of the two sent", stored == "1", f"got {stored}")

# ------------------------------------------------------------ append-only
err = q_fails(f"""update interaction_log set event='granted'
                   where gate_id='{GATE}' and seq=3;""")
check("an event cannot be edited", "append-only" in err, err)

err = q_fails(f"delete from interaction_log where gate_id='{GATE}' and seq=3;")
check("an event cannot be deleted directly",
      "purge_interaction_log" in err, err)

# ------------------------------------------------------------- the holes
# Events 5..9 were overwritten in the ring before they could be sent.
ingest(events(
    (10, T0 + 100, "granted", 0, "a1b2c3d4", True),
), dropped=6)

gap = q(f"""select missing_from || '-' || missing_to || ' (' || missing_count || ')'
              from interaction_gap where gate_id='{GATE}';""")
check("a gap in the sequence is visible", gap == "5-9 (5)", f"got {gap}")

drop = q("select dropped from interaction_upload order by id desc limit 1;")
check("and the device's dropped counter is recorded", drop == "6", f"got {drop}")

# ------------------------------------------------- two timestamps, never one
row = q(f"""select clock_trusted,
                   floor(extract(epoch from device_time))::bigint,
                   received_at > device_time
              from interaction_log where gate_id='{GATE}' and seq=1;""")
trusted, dev_epoch, later = row.split("|")
check("the device's clock and ours are both kept, and differ",
      trusted == "f" and int(dev_epoch) == T0 + 10 and later == "t", row)

caveat = q(f"""select caveat from interaction_log_readable
                where gate_id='{GATE}' and seq=1;""")
check("an untrusted clock is flagged for the reader",
      caveat == "clock untrusted", f"got {caveat!r}")

# ------------------------------------------------------------- readability
detail = q(f"""select detail from interaction_log_readable
                 where gate_id='{GATE}' and seq=3;""")
check("a denial says why in words", detail == "unknown code", f"got {detail!r}")

boot = q(f"""select detail from interaction_log_readable
               where gate_id='{GATE}' and seq=1;""")
check("a boot says what reset it", boot == "reset: task watchdog", f"got {boot!r}")

# --------------------------------------------------- constraints that bite
err = q_fails(f"""select ingest_log_batch('{GATE}',
    '[{{"seq":99,"ts":{T0},"event":"denied","result":null,"clock_trusted":true}}]'::jsonb);""")
check("a denial with no reason is refused",
      "denial_carries_a_reason" in err, err)

err = q_fails(f"""select ingest_log_batch('{GATE}',
    '[{{"seq":98,"ts":{T0},"event":"exploded","clock_trusted":true}}]'::jsonb);""")
check("an unknown event type is refused",
      "interaction_log_event_check" in err, err)

err = q_fails("""select ingest_log_batch('portao-99',
    '[{"seq":1,"ts":1,"event":"boot","clock_trusted":false}]'::jsonb);""")
check("a batch for an unknown gate is refused", "no such gate" in err, err)

# ------------------------------------------- the log outlives what it describes
# An audit record has to survive the deletion of the code it names.
q("""insert into access_code (code_id, gate_id, code_hash, valid_from,
       valid_until, slot)
     values ('a1b2c3d4', 'portao-01', decode(repeat('aa',32),'hex'),
             to_timestamp(1800000000), to_timestamp(1900000000), 0)
     on conflict do nothing;""")
q("set session gate.retention = 'off';")
q("delete from access_code where code_id='a1b2c3d4';")
left = q(f"""select count(*) from interaction_log
              where gate_id='{GATE}' and code_id='a1b2c3d4';""")
check("deleting a code does not take the log with it", left == "2", f"got {left}")

# -------------------------------------------------------------- retention
err = q_fails("select purge_interaction_log(interval '1 hour');")
check("the purge refuses a reckless window", "younger than a day" in err, err)

before = int(q(f"select count(*) from interaction_log where gate_id='{GATE}';"))
gone = int(q("select purge_interaction_log(interval '1 day');"))
check("nothing recent is purged", gone == 0, f"removed {gone}")

q(f"""set session gate.retention = 'on';
      insert into interaction_log (gate_id, seq, device_time, clock_trusted,
                                   event, received_at)
      values ('{GATE}', 500, to_timestamp({T0}), true, 'boot',
              now() - interval '400 days');""")
gone = int(q("select purge_interaction_log(interval '365 days');"))
after = int(q(f"select count(*) from interaction_log where gate_id='{GATE}';"))
check("an old row is purged and the rest survives",
      gone == 1 and after == before, f"removed {gone}, {before} -> {after}")

# The permission to delete must not leak out of the purge.
err = q_fails(f"delete from interaction_log where gate_id='{GATE}' and seq=1;")
check("and the permission does not leak past it",
      "purge_interaction_log" in err, err)

sys.exit(summary())
