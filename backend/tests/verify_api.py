#!/usr/bin/env python3
"""Drive the API against a real database.

Not mocked: the app talks to Postgres, the MACs are computed with the
same key the database holds, and the bodies are the ones the firmware
would send and receive.

    DATABASE_URL=postgresql://gate:...@localhost:5432/gate \\
    ADMIN_TOKEN=... python3 backend/tests/verify_api.py
"""
from __future__ import annotations

import asyncio
import hashlib
import hmac
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import httpx  # noqa: E402

from app import main as api  # noqa: E402
from pg import GATE, OTHER_GATE, banner, check, q, summary  # noqa: E402

banner()

SALT = "api-test-salt-not-a-real-one"
KEY = "3c" * 32
TOKEN = os.environ.setdefault("ADMIN_TOKEN", "test-token-for-the-bench")

api.config.admin_token = TOKEN


def mac(signed: str) -> str:
    return hmac.new(bytes.fromhex(KEY), signed.encode(), hashlib.sha256).hexdigest()


def batch(gate: str, dropped: int, events: list[str]) -> str:
    body = f"portao-log v1\ngate {gate}\ndropped {dropped}\n"
    for e in events:
        body += f"event {e}\n"
    body += "end\n"
    return body + f"mac {mac(body)}\n"


# ---------------------------------------------------------------- setup
q(f"insert into gate (gate_id, label) values ('{GATE}', 'api bench');")
q(f"""insert into gate_secret (gate_id, device_salt, delivery_key)
      values ('{GATE}', '{SALT}', decode('{KEY}', 'hex'));""")

T0 = int(time.time()) - 3600


async def main() -> int:
    transport = httpx.ASGITransport(app=api.app)
    async with api.app.router.lifespan_context(api.app):
        async with httpx.AsyncClient(transport=transport,
                                     base_url="http://bench") as c:

            # ----------------------------------------------- the poll
            r = await c.get(f"/gate/{GATE}.txt")
            check("the poll returns a signed body", r.status_code == 200
                  and r.text.startswith("portao-sync v1\n")
                  and r.text.rstrip().endswith(mac_of_sync(r.text)),
                  f"{r.status_code}: {r.text[:120]!r}")

            check("and it is served as plain text",
                  r.headers["content-type"].startswith("text/plain"),
                  r.headers.get("content-type", ""))

            seen = q(f"""select poll_count, last_generation from gate_seen
                          where gate_id='{GATE}';""")
            check("the poll is recorded, so a silent gate can be noticed",
                  seen.startswith("1|"), f"got {seen!r}")

            r = await c.get(f"/gate/{OTHER_GATE}-missing.txt")
            check("an unknown gate is 404", r.status_code == 404, str(r.status_code))

            # ---------------------------------------------- the ingest
            good = batch(GATE, 0, [
                f"1 {T0 + 10} boot 6 - 0",
                f"2 {T0 + 20} granted 0 a1b2c3d4 1",
                f"3 {T0 + 30} denied 1 - 1",
            ])
            r = await c.post(f"/gate/{GATE}/log", content=good)
            check("a signed batch is accepted", r.status_code == 200,
                  f"{r.status_code}: {r.text[:200]!r}")
            check("and answered with a signed ack for the highest seq",
                  r.text.startswith("portao-ack v1\n")
                  and "acked 3\n" in r.text
                  and ack_verifies(r.text),
                  repr(r.text))

            n = q(f"select count(*) from interaction_log where gate_id='{GATE}';")
            check("the events landed", n == "3", f"got {n}")

            r2 = await c.post(f"/gate/{GATE}/log", content=good)
            n2 = q(f"select count(*) from interaction_log where gate_id='{GATE}';")
            check("re-sending the same batch changes nothing",
                  r2.status_code == 200 and n2 == "3" and "acked 3" in r2.text,
                  f"{r2.status_code}, rows {n2}")

            # ------------------------------------------- the refusals
            forged = good[:-65] + ("0" * 64) + "\n"
            r = await c.post(f"/gate/{GATE}/log", content=forged)
            check("a wrong MAC is refused", r.status_code == 403, str(r.status_code))

            tampered = good.replace("granted", "lockout", 1)
            r = await c.post(f"/gate/{GATE}/log", content=tampered)
            check("changing one field breaks the signature",
                  r.status_code == 403, str(r.status_code))

            # Cut after the second event: the MAC is still the one the
            # device computed, but it covered an "end" that is now gone.
            truncated = "\n".join(good.split("\n")[:4]) + "\n"
            r = await c.post(f"/gate/{GATE}/log", content=truncated)
            check("a truncated body cannot verify", r.status_code == 400,
                  f"{r.status_code}: {r.text[:120]}")

            # Correctly signed with this gate's key, addressed elsewhere.
            misaddressed = batch(OTHER_GATE, 0, [f"9 {T0} boot 1 - 1"])
            r = await c.post(f"/gate/{GATE}/log", content=misaddressed)
            check("a batch addressed to another gate is refused",
                  r.status_code == 400 and "another gate" in r.text,
                  f"{r.status_code}: {r.text[:120]}")

            denial = batch(GATE, 0, [f"20 {T0} denied - - 1"])
            r = await c.post(f"/gate/{GATE}/log", content=denial)
            check("a denial with no reason is refused",
                  r.status_code == 400 and "must say why" in r.text,
                  f"{r.status_code}: {r.text[:120]}")

            r = await c.post(f"/gate/{GATE}/log", content="x" * 9000)
            check("an oversized body is refused before parsing",
                  r.status_code == 400, str(r.status_code))

            # -------------------------------------------- the holes
            gapped = batch(GATE, 7, [f"11 {T0 + 100} granted 0 a1b2c3d4 1"])
            r = await c.post(f"/gate/{GATE}/log", content=gapped)
            gap = q(f"""select missing_from || '-' || missing_to
                          from interaction_gap where gate_id='{GATE}';""")
            drop = q("select dropped from interaction_upload order by id desc limit 1;")
            check("a gap and the dropped counter both survive the round trip",
                  r.status_code == 200 and gap == "4-10" and drop == "7",
                  f"gap {gap!r}, dropped {drop!r}")

            # --------------------------------------------- the admin
            r = await c.post(f"/admin/gates/{GATE}/codes",
                             json={"valid_from": "2027-01-01T00:00:00Z",
                                   "valid_until": "2027-01-05T00:00:00Z",
                                   "note": "guest A"})
            check("issuing without a token is refused", r.status_code == 401,
                  str(r.status_code))

            auth = {"Authorization": f"Bearer {TOKEN}"}
            r = await c.post(f"/admin/gates/{GATE}/codes", headers=auth,
                             json={"valid_from": "2027-01-01T00:00:00Z",
                                   "valid_until": "2027-01-05T00:00:00Z",
                                   "note": "guest A"})
            check("issuing with one works", r.status_code == 200,
                  f"{r.status_code}: {r.text[:200]}")
            issued = r.json()

            plain = issued["plaintext"]
            digits = sum(ch.isdigit() for ch in plain)
            check("the code matches format_version 1",
                  len(plain) == 9 and digits == 6
                  and sum(ch in "ABCD" for ch in plain) == 2
                  and sum(ch in "*#" for ch in plain) == 1,
                  f"got {plain!r}")

            want = hashlib.sha256((plain + SALT).encode()).hexdigest()
            got = q(f"""select encode(code_hash,'hex') from access_code
                         where code_id='{issued['code_id']}';""")
            check("and only its hash is stored", got == want, f"{got} vs {want}")

            stored = q(f"""select count(*) from access_code
                            where code_id='{issued['code_id']}'
                              and {_no_plaintext_column()};""")
            check("there is nowhere a plaintext could have been stored",
                  stored == "1", f"got {stored}")

            r = await c.get(f"/gate/{GATE}.txt")
            check("the new code appears in the next poll",
                  issued["code_id"] in r.text, r.text[:200])

            r = await c.delete(f"/admin/codes/{issued['code_id']}", headers=auth)
            check("revoking works", r.status_code == 200, r.text[:120])

            r = await c.delete(f"/admin/codes/{issued['code_id']}", headers=auth)
            check("revoking twice is a 404, not a silent success",
                  r.status_code == 404, str(r.status_code))

            r = await c.get(f"/gate/{GATE}.txt")
            check("and it is gone from the poll",
                  issued["code_id"] not in r.text, r.text[:200])

            # ------------------------------------------ the admin log
            r = await c.get(f"/admin/gates/{GATE}/log", headers=auth)
            body = r.json()
            kinds = {e["detail"] for e in body["events"] if e["detail"]}
            check("the admin log reads in words",
                  r.status_code == 200
                  and "unknown code" in kinds
                  and "reset: task watchdog" in kinds
                  and body["gaps"][0]["missing_count"] == 7,
                  f"{r.status_code}: {kinds}")

            r = await c.get("/healthz")
            check("healthz talks to the database", r.status_code == 200,
                  str(r.status_code))

    return summary()


def mac_of_sync(body: str) -> str:
    signed = body.split("mac ")[0]
    return mac(signed)


def ack_verifies(body: str) -> bool:
    signed, _, rest = body.partition("mac ")
    return rest.strip() == mac(signed)


def _no_plaintext_column() -> str:
    cols = q("""select string_agg(column_name, ',') from information_schema.columns
                 where table_name='access_code';""").split(",")
    bad = [c for c in cols if "plain" in c or "secret" in c or c == "code"]
    assert not bad, f"access_code grew a column that could hold a code: {bad}"
    return "true"


sys.exit(asyncio.run(main()))
