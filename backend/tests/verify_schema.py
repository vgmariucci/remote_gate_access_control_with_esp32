#!/usr/bin/env python3
"""Check the schema against the firmware's own rules.

The body gate_sync_body() returns must be byte-identical to what
tools/make_sync_file.py emits, because that generator is the one the
device-side path was verified against on real hardware.
"""
import hashlib
import hmac
import os
import shutil
import subprocess
import sys
from pathlib import Path

# Ordinary libpq environment: PGHOST, PGPORT, PGUSER, PGPASSWORD all
# work as usual. Only the names are defaulted, and only when nothing
# has set them.
ENV = dict(os.environ)
ENV.setdefault("PGDATABASE", "gate")
ENV.setdefault("PGUSER", "gate")

COMPOSE = Path(__file__).resolve().parents[2] / "docker" / "compose.yaml"


def psql_argv() -> tuple[list[str], str]:
    """Find a psql, one way or another.

    There are two reasonable places for one: installed on the host, or
    already inside the database container. Running it in the container
    is the default when there is a compose file and nothing in the
    environment points somewhere else, because then the development
    database needs no host package at all - and because a local socket
    inside that image trusts you, so there is no password to arrange.

    Set PGHOST (or VERIFY_VIA=psql) to use a host psql instead.
    """
    want = os.environ.get("VERIFY_VIA")

    if want not in ("psql", "docker", None):
        raise SystemExit(f"VERIFY_VIA must be 'psql' or 'docker', not {want!r}")

    use_docker = (want == "docker") or (
        want is None and "PGHOST" not in os.environ and COMPOSE.exists()
    )

    if use_docker:
        if not shutil.which("docker"):
            raise SystemExit("no docker on PATH, and no PGHOST set")
        return ([
            "docker", "compose", "-f", str(COMPOSE), "exec", "-T", "db",
            "psql", "-U", ENV["PGUSER"], "-d", ENV["PGDATABASE"],
        ], f"docker compose exec db psql ({COMPOSE})")

    if not shutil.which("psql"):
        raise SystemExit(
            "no psql on PATH and no container to borrow one from.\n"
            "  either:  docker compose -f docker/compose.yaml up -d db\n"
            "  or:      sudo apt install postgresql-client-16"
        )
    return (["psql"], "psql on PATH")


PSQL, TRANSPORT = psql_argv()

SALT = "dev-salt-for-the-bench-only"
KEY = "9f" * 32
FAILURES = []


def _run(sql: str) -> subprocess.CompletedProcess:
    # -X so a developer's ~/.psqlrc cannot change the output format out
    # from under the parsing below.
    return subprocess.run(PSQL + ["-X", "-tAq", "-v", "ON_ERROR_STOP=1", "-c", sql],
                          env=ENV, capture_output=True, text=True)


def q(sql: str) -> str:
    r = _run(sql)
    if r.returncode != 0:
        raise RuntimeError(r.stderr.strip())
    return r.stdout.rstrip("\n")


def q_fails(sql: str) -> str:
    r = _run(sql)
    if r.returncode == 0:
        raise RuntimeError(f"expected a failure, got success: {sql}")
    return r.stderr.strip()


print(f"via {TRANSPORT}\n")


def check(name: str, ok: bool, detail: str = "") -> None:
    print(f"{'ok  ' if ok else 'FAIL'}  {name}" + (f"\n        {detail}" if detail and not ok else ""))
    if not ok:
        FAILURES.append(name)


# ---------------------------------------------------------------- setup
q("delete from gate;")
q(f"insert into gate (gate_id, label) values ('portao-01', 'bench');")
q(f"""insert into gate_secret (gate_id, device_salt, delivery_key)
      values ('portao-01', '{SALT}', decode('{KEY}', 'hex'));""")

# ---------------------------------------------------- format_version 1
# Exactly 9 chars: 6 digits, 2 of A-D, 1 of * #. ac_format_valid().
bad = []
seen_special_pos = set()
for _ in range(400):
    c = q("select generate_code_v1();")
    digits = sum(ch.isdigit() for ch in c)
    letters = sum(ch in "ABCD" for ch in c)
    specials = sum(ch in "*#" for ch in c)
    if not (len(c) == 9 and digits == 6 and letters == 2 and specials == 1):
        bad.append(c)
    seen_special_pos.add(next(i for i, ch in enumerate(c) if ch in "*#"))
check("generate_code_v1 matches ac_format_valid (400 draws)", not bad,
      f"bad: {bad[:3]}")
check("the special lands in every position, so the shuffle runs",
      len(seen_special_pos) == 9, f"positions seen: {sorted(seen_special_pos)}")

# ------------------------------------------------------- generation
gen0 = int(q("select generation from gate where gate_id='portao-01';"))
check("a fresh gate starts at generation 0", gen0 == 0, f"got {gen0}")

issued = []
for i in range(3):
    row = q(f"""select code_id, plaintext from issue_code(
                 'portao-01',
                 to_timestamp({1800000000 + i}),
                 to_timestamp({1900000000 + i}),
                 'guest {i}');""")
    code_id, plain = row.split("|")
    issued.append((code_id, plain, 1800000000 + i, 1900000000 + i))

gen3 = int(q("select generation from gate where gate_id='portao-01';"))
check("three inserts move the generation three times", gen3 == 3, f"got {gen3}")

# -------------------------------------------- the body, byte for byte
body = q("select gate_sync_body('portao-01');")

rows = q("""select code_id, encode(code_hash,'hex'),
                   floor(extract(epoch from valid_from))::bigint,
                   floor(extract(epoch from valid_until))::bigint
              from gate_live_code where gate_id='portao-01' order by slot;""")

lines = ["portao-sync v1", "gate portao-01", f"gen {gen3}"]
for r in rows.split("\n"):
    cid, h, f, u = r.split("|")
    lines.append(f"code {cid} {h} {f} {u}")
lines.append("end")
signed = "\n".join(lines) + "\n"
mac = hmac.new(bytes.fromhex(KEY), signed.encode(), hashlib.sha256).hexdigest()
expected = signed + f"mac {mac}\n"

check("gate_sync_body is byte-identical to the make_sync_file.py layout",
      body + "\n" == expected or body == expected.rstrip("\n"),
      f"got:\n{body!r}\nwant:\n{expected!r}")

# The hash really is sha256(plaintext || salt) - the thing that, if
# wrong, signs perfectly and then refuses the code at the keypad.
cid, plain, _, _ = issued[0]
want = hashlib.sha256(plain.encode() + SALT.encode()).hexdigest()
got = q(f"select encode(code_hash,'hex') from access_code where code_id='{cid}';")
check("code_hash is sha256(plaintext || device_salt)", got == want,
      f"got {got}\nwant {want}")

# Tamper with one byte: the MAC must stop verifying.
tampered = expected.replace("portao-01", "portao-02", 1)
sig = tampered.split("mac ")[0]
check("a one-field change breaks the MAC",
      hmac.new(bytes.fromhex(KEY), sig.encode(),
               hashlib.sha256).hexdigest() != mac)

# --------------------------------------------------- the five slots
for i in range(3, 5):
    q(f"""select code_id from issue_code('portao-01',
          to_timestamp({1800000000 + i}), to_timestamp({1900000000 + i}));""")
err = q_fails("select code_id from issue_code('portao-01', now(), now() + interval '1 day');")
check("a sixth live code is refused", "five live codes" in err, err)

slots = q("select string_agg(slot::text, ',' order by slot) from gate_live_code where gate_id='portao-01';")
check("the five seats are 0..4", slots == "0,1,2,3,4", f"got {slots}")

# Revoking frees exactly that seat, and moves the generation.
before = int(q("select generation from gate where gate_id='portao-01';"))
q(f"select revoke_code('{issued[1][0]}');")
after = int(q("select generation from gate where gate_id='portao-01';"))
check("revoking moves the generation", after == before + 1, f"{before} -> {after}")
free = q("""select n from generate_series(0,4) n where not exists
            (select 1 from access_code c where c.gate_id='portao-01'
             and c.revoked_at is null and c.slot=n);""")
check("revoking frees the seat it held", free == "1", f"free seat: {free}")

# ------------------------------------------- windows and reaping
q("""insert into access_code (code_id, gate_id, code_hash, valid_from,
       valid_until, slot)
     values ('deadbeef', 'portao-01', decode(repeat('aa',32),'hex'),
             now() - interval '2 day', now() - interval '1 day', 1);""")
inset = q("select count(*) from gate_live_code where code_id='deadbeef';")
check("an expired code is not in the live set", inset == "0", f"got {inset}")
held = q("""select count(*) from access_code where gate_id='portao-01'
            and revoked_at is null;""")
check("but it still holds its seat until reaped", held == "5", f"got {held}")
reaped = q("select reap_expired_codes();")
check("reap_expired_codes frees it", reaped == "1", f"got {reaped}")

err = q_fails("""insert into access_code (code_id, gate_id, code_hash,
                   valid_from, valid_until, slot)
                 values ('ffffffff','portao-01', decode(repeat('bb',32),'hex'),
                         now(), now() - interval '1 hour', 1);""")
check("an inverted window cannot be stored", "window_runs_forwards" in err, err)

err = q_fails("""insert into access_code (code_id, gate_id, code_hash,
                   valid_from, valid_until, slot)
                 values ('NOTHEX00','portao-01', decode(repeat('bb',32),'hex'),
                         now(), now()+interval '1 day', 1);""")
check("a non-hex id cannot be stored", "code_id_check" in err, err)

# --------------------------------------------- cross-gate movement
q("insert into gate (gate_id) values ('portao-02');")
err = q_fails(f"update access_code set gate_id='portao-02' where code_id='{issued[0][0]}';")
check("a code cannot move between gates", "cannot move between gates" in err, err)

# ------------------------------------------------ the empty set
# ADR 0007: an empty set is honoured, and is how mass revocation works.
q("update access_code set revoked_at = now() where gate_id='portao-01' and revoked_at is null;")
empty = q("select gate_sync_body('portao-01');")
gen_now = int(q("select generation from gate where gate_id='portao-01';"))
sig = f"portao-sync v1\ngate portao-01\ngen {gen_now}\nend\n"
want_mac = hmac.new(bytes.fromhex(KEY), sig.encode(), hashlib.sha256).hexdigest()
check("an empty set still produces a valid signed body",
      empty.rstrip("\n") == (sig + f"mac {want_mac}").rstrip("\n"),
      f"got:\n{empty!r}")

# ------------------------------------------------- the tg counter
c1 = int(q("select next_tg_counter('portao-01');"))
c2 = int(q("select next_tg_counter('portao-01');"))
check("the tg counter is monotonic", c2 == c1 + 1, f"{c1}, {c2}")

# ------------------------------------------------------- body size
# SYNC_BODY_MAX is 2048; more than 8 entries is refused outright.
q("delete from access_code where gate_id='portao-01';")
for i in range(5):
    q(f"""select code_id from issue_code('portao-01',
          to_timestamp(1800000000), to_timestamp(1999999999));""")
size = len(q("select gate_sync_body('portao-01');").encode()) + 1
check("a full five-code body fits SYNC_BODY_MAX (2048)", size < 2048,
      f"{size} bytes")
print(f"\n      a full body is {size} bytes of the 2048 the device accepts")

print()
if FAILURES:
    print(f"{len(FAILURES)} failure(s): {', '.join(FAILURES)}")
    sys.exit(1)
print("all checks passed")