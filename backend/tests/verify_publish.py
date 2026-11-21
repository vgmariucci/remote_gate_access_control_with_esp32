#!/usr/bin/env python3
"""Check that the bucket cannot go quietly stale.

The interesting cases are all failures, so the uploader is a fake that
can be switched off. A publisher tested only on the happy path tells
you nothing about the state this whole mechanism exists for: a code
that is committed, valid, and invisible to the gate.
"""
from __future__ import annotations

import asyncio
import hashlib
import hmac
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import httpx  # noqa: E402

from app import main as api  # noqa: E402
from app import publish as pub  # noqa: E402
from pg import GATE, banner, check, q, summary  # noqa: E402

banner()

SALT = "publish-test-salt-not-real"
KEY = "7e" * 32
TOKEN = os.environ.setdefault("ADMIN_TOKEN", "test-token-for-the-bench")
api.config.admin_token = TOKEN

BUCKET = "bench-bucket"


class FakeS3:
    """An object store with a switch on it."""

    bucket = BUCKET

    def __init__(self) -> None:
        self.objects: dict[str, bytes] = {}
        self.puts = 0
        self.fail_with: Exception | None = None

    def put(self, key: str, body: bytes) -> str:
        self.puts += 1
        if self.fail_with is not None:
            raise self.fail_with
        self.objects[key] = body
        return hashlib.md5(body).hexdigest()  # noqa: S324 - an ETag, not a secret


def mac(signed: str) -> str:
    return hmac.new(bytes.fromhex(KEY), signed.encode(), hashlib.sha256).hexdigest()


q(f"insert into gate (gate_id, label) values ('{GATE}', 'publish bench');")
q(f"""insert into gate_secret (gate_id, device_salt, delivery_key)
      values ('{GATE}', '{SALT}', decode('{KEY}', 'hex'));""")

KEY_NAME = f"gate-{GATE}.txt"
AUTH = {"Authorization": f"Bearer {TOKEN}"}


def drift_state() -> str:
    return q(f"select state from gate_drift where gate_id = '{GATE}';")


def live_body(s3: FakeS3) -> str:
    return s3.objects.get(KEY_NAME, b"").decode()


async def main() -> int:
    s3 = FakeS3()
    api.uploader = s3

    transport = httpx.ASGITransport(app=api.app)
    async with api.app.router.lifespan_context(api.app):
        api.uploader = s3                      # the lifespan reads the env
        async with httpx.AsyncClient(transport=transport,
                                     base_url="http://bench") as c:

            check("a gate that has never been published says so",
                  drift_state() == "never published", drift_state())

            # ------------------------------------------- the happy path
            r = await c.post(f"/admin/gates/{GATE}/codes", headers=AUTH,
                             json={"valid_from": "2027-01-01T00:00:00Z",
                                   "valid_until": "2027-01-05T00:00:00Z",
                                   "note": "guest A"})
            first = r.json()
            check("issuing publishes, and says it did",
                  r.status_code == 200 and first["published"] is True,
                  f"{r.status_code}: {r.text[:200]}")

            check("the bucket holds the key the device fetches",
                  KEY_NAME in s3.objects, str(list(s3.objects)))

            body = live_body(s3)
            check("and what it holds is the signed body, verbatim",
                  body.startswith("portao-sync v1\n")
                  and first["code_id"] in body
                  and body.rstrip().endswith(mac(body.split("mac ")[0])),
                  body[:160])

            check("the database agrees the bucket is current",
                  drift_state() == "current", drift_state())

            # ------------------------------------ the failure that matters
            s3.fail_with = ConnectionError("bucket unreachable")
            r = await c.post(f"/admin/gates/{GATE}/codes", headers=AUTH,
                             json={"valid_from": "2027-02-01T00:00:00Z",
                                   "valid_until": "2027-02-05T00:00:00Z",
                                   "note": "guest B"})
            second = r.json()
            check("a failed upload does not fail the request",
                  r.status_code == 200, f"{r.status_code}: {r.text[:200]}")
            check("but it is reported rather than swallowed",
                  second["published"] is False
                  and "bucket unreachable" in second["publish_detail"],
                  str(second))

            check("and the gate is now visibly behind",
                  drift_state() == "behind", drift_state())

            err = q(f"""select last_error from gate_publication
                         where gate_id = '{GATE}';""")
            check("the reason is kept until the next success",
                  "bucket unreachable" in err, err)

            check("the bucket still holds the older body - not a corrupt one",
                  second["code_id"] not in live_body(s3)
                  and first["code_id"] in live_body(s3),
                  live_body(s3)[:160])

            r = await c.get("/admin/drift", headers=AUTH)
            check("drift is one query away",
                  r.status_code == 200 and r.json()["stale"] == 1,
                  r.text[:200])

            # ----------------------------------------- the reconciler
            s3.fail_with = None
            r = await c.post("/admin/publish", headers=AUTH)
            check("the reconciler repairs it",
                  r.status_code == 200 and r.json()["published"] == [GATE]
                  and not r.json()["failed"],
                  r.text[:200])

            check("and the second code is live at last",
                  second["code_id"] in live_body(s3), live_body(s3)[:200])
            check("the error is cleared on success",
                  q(f"""select last_error from gate_publication
                         where gate_id='{GATE}';""") == "", "still set")

            r = await c.post("/admin/publish", headers=AUTH)
            check("a reconcile with nothing to do does nothing",
                  r.json()["published"] == [], r.text[:120])

            # --------------------------------- revocation must reach it
            before = s3.puts
            r = await c.delete(f"/admin/codes/{second['code_id']}", headers=AUTH)
            check("revoking publishes too",
                  r.status_code == 200 and r.json()["published"] is True
                  and s3.puts > before, r.text[:200])
            check("and the revoked code is gone from the bucket",
                  second["code_id"] not in live_body(s3), live_body(s3)[:200])

            # ------------------------------------------ out of order
            # Two publishers can finish in the wrong order. The table
            # must keep the newest generation it knows was written.
            gen = int(q(f"select generation from gate where gate_id='{GATE}';"))
            recorded = q(f"""select note_published('{GATE}', {gen - 1},
                             '{BUCKET}', '{KEY_NAME}', 'stale');""")
            check("a late, older publish cannot rewind the record",
                  recorded == "f"
                  and int(q(f"""select generation from gate_publication
                                 where gate_id='{GATE}';""")) == gen,
                  f"recorded={recorded}")

            # -------------------------------- nothing configured at all
            api.uploader = None
            r = await c.post(f"/admin/gates/{GATE}/publish", headers=AUTH)
            check("with no bucket configured it says so plainly",
                  r.status_code == 200 and r.json()["published"] is False
                  and r.json()["detail"] == "no bucket configured",
                  r.text[:200])
            api.uploader = s3

            r = await c.post(f"/admin/gates/{GATE}-nope/publish", headers=AUTH)
            check("publishing an unknown gate is a 404",
                  r.status_code == 404, str(r.status_code))

    return summary()


# The key the device fetches is part of the contract with the bucket
# policy, which is scoped to gate-*.txt. Worth a check of its own,
# because getting it wrong produces a 404 at the gate and nothing here.
check("the object key matches what the device's URL expects",
      pub.KEY_TEMPLATE.format(gate_id="portao-01") == "gate-portao-01.txt",
      pub.KEY_TEMPLATE)

sys.exit(asyncio.run(main()))
