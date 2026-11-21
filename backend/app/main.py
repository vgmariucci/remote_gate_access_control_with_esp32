"""The gate backend.

Three audiences, and they want very different things:

  the device   GET  /gate/{gate_id}.txt        the signed code set
               POST /gate/{gate_id}/log        a signed batch of events
  an admin     POST   /admin/gates/{id}/codes  issue a code
               DELETE /admin/codes/{code_id}   revoke one
               GET    /admin/gates/{id}/log    read the trail
  a monitor    GET  /healthz

The device routes carry no credential. They do not need one: every byte
in either direction is covered by an HMAC with that gate's delivery
key, and the key never leaves Postgres - this process passes bytes to
`verify_log_mac` and `gate_ack_body` and gets back a verdict or a
signed reply. An API that cannot hold a key cannot leak one.
"""
from __future__ import annotations

import logging
from contextlib import asynccontextmanager
from datetime import datetime

from fastapi import Depends, FastAPI, Header, HTTPException, Request, Response
from psycopg.rows import dict_row
from psycopg.types.json import Jsonb
from psycopg_pool import AsyncConnectionPool
from pydantic import BaseModel, Field

from .config import config
from .publish import S3Uploader, publish_gate, reconcile
from .wire import WireError, parse, split_signed

log = logging.getLogger("gate")

pool: AsyncConnectionPool | None = None
uploader: S3Uploader | None = None


@asynccontextmanager
async def lifespan(app: FastAPI):
    global pool, uploader
    if not config.admin_token:
        raise RuntimeError(
            "ADMIN_TOKEN is not set. The admin routes would be unguarded; "
            "refusing to start."
        )
    pool = AsyncConnectionPool(
        config.dsn, min_size=config.pool_min, max_size=config.pool_max,
        open=False, kwargs={"row_factory": dict_row},
    )
    await pool.open(wait=True, timeout=10)

    uploader = S3Uploader.from_env()
    if uploader is None:
        # Not fatal - a bench has no bucket. But say it out loud,
        # because without one the gate's correctness path runs through
        # this process, which is the thing publishing exists to avoid.
        log.warning("S3_BUCKET is not set: nothing will be published, "
                    "and a device polling the bucket will not see changes")
    else:
        log.info("publishing to s3://%s/%s", uploader.bucket, uploader.prefix or "")

    try:
        yield
    finally:
        await pool.close()


app = FastAPI(title="portao gate backend", version="1", lifespan=lifespan)


async def fetchone(sql: str, *args):
    assert pool is not None
    async with pool.connection() as conn:
        async with conn.cursor() as cur:
            await cur.execute(sql, args)
            return await cur.fetchone()


async def fetchall(sql: str, *args):
    assert pool is not None
    async with pool.connection() as conn:
        async with conn.cursor() as cur:
            await cur.execute(sql, args)
            return await cur.fetchall()


def client_addr(request: Request) -> str | None:
    """The caller's address, if we are entitled to believe it.

    X-Forwarded-For is trivially spoofable unless something in front is
    overwriting it, so it counts only when the operator says so. A
    wrong address in an audit trail is worse than a missing one.
    """
    if config.trust_forwarded:
        fwd = request.headers.get("x-forwarded-for")
        if fwd:
            return fwd.split(",")[0].strip()
    return request.client.host if request.client else None


def require_admin(authorization: str = Header(default="")) -> None:
    import hmac as _hmac

    expected = f"Bearer {config.admin_token}"
    # Constant time, so the token cannot be recovered a character at a
    # time from response latency.
    if not _hmac.compare_digest(authorization, expected):
        raise HTTPException(status_code=401, detail="unauthorized")


# ---------------------------------------------------------------------
# The device
# ---------------------------------------------------------------------

@app.get("/gate/{gate_id}.txt", response_class=Response)
async def sync_body(gate_id: str, request: Request) -> Response:
    """The signed code set.

    The device sends nothing: no query string, no headers, no
    credential. It fetches this whole body every time and compares the
    `gen` line to what it holds. So this must not require anything the
    device will never send.
    """
    row = await fetchone("select gate_sync_body(%s) as body", gate_id)
    if row is None or row["body"] is None:
        raise HTTPException(status_code=404, detail="no such gate")

    body: str = row["body"]

    # The device reports nothing about itself, so its polls are the only
    # evidence it still exists. This is the one place "has that gate
    # gone quiet?" can be answered from.
    generation = 0
    for line in body.split("\n"):
        if line.startswith("gen "):
            generation = int(line[4:])
            break
    await fetchone(
        "select note_poll(%s, %s, %s, %s)",
        gate_id, generation, client_addr(request),
        request.headers.get("user-agent", "")[:200],
    )

    return Response(content=body, media_type="text/plain; charset=utf-8")


@app.post("/gate/{gate_id}/log", response_class=Response)
async def ingest(gate_id: str, request: Request) -> Response:
    """A batch of events, signed, answered with a signed acknowledgement.

    Order matters and is the same one tg_proto uses: find the signed
    region, authenticate the whole of it, and only then believe a
    single field. A forged body must not be able to steer the parser
    with its contents.
    """
    raw = await request.body()
    try:
        body = raw.decode("utf-8")
    except UnicodeDecodeError:
        raise HTTPException(status_code=400, detail="not utf-8")

    try:
        signed, mac_hex = split_signed(body)
    except WireError as e:
        raise HTTPException(status_code=400, detail=str(e))

    row = await fetchone(
        "select verify_log_mac(%s, %s, %s) as ok", gate_id, signed, mac_hex
    )
    if not row or not row["ok"]:
        # Deliberately indistinguishable from a wrong key: a caller who
        # cannot sign learns nothing about whether the gate exists.
        raise HTTPException(status_code=403, detail="bad signature")

    try:
        batch = parse(signed, gate_id)
    except WireError as e:
        raise HTTPException(status_code=400, detail=str(e))

    acked = await fetchone(
        "select ingest_log_batch(%s, %s, %s, %s) as acked",
        gate_id, Jsonb([e.as_row() for e in batch.events]),
        batch.dropped, client_addr(request),
    )

    ack = await fetchone("select gate_ack_body(%s, %s) as body",
                         gate_id, acked["acked"])
    return Response(content=ack["body"], media_type="text/plain; charset=utf-8")


# ---------------------------------------------------------------------
# An admin
# ---------------------------------------------------------------------

class IssueRequest(BaseModel):
    valid_from: datetime
    valid_until: datetime
    note: str = Field(default="", max_length=500)
    created_by: str = Field(default="", max_length=100)


class IssueResponse(BaseModel):
    code_id: str
    plaintext: str
    slot: int
    # False means the code exists and the gate cannot see it yet.
    published: bool = False
    publish_detail: str = ""


@app.post("/admin/gates/{gate_id}/codes", response_model=IssueResponse,
          dependencies=[Depends(require_admin)])
async def issue(gate_id: str, req: IssueRequest) -> IssueResponse:
    """Issue a code. The plaintext is returned once and never stored.

    Give it to the guest now; nothing here or on the gate can recover
    it afterwards.
    """
    if req.valid_until < req.valid_from:
        raise HTTPException(status_code=400, detail="the window ends before it starts")

    try:
        row = await fetchone(
            "select * from issue_code(%s, %s, %s, %s, %s)",
            gate_id, req.valid_from, req.valid_until, req.note, req.created_by,
        )
    except Exception as e:
        # issue_code raises for an unknown gate and for a full table.
        # Both are the caller's problem, not a server fault.
        detail = str(e).strip().splitlines()[0]
        if "no such gate" in detail:
            raise HTTPException(status_code=404, detail="no such gate")
        if "five live codes" in detail:
            raise HTTPException(status_code=409, detail=detail)
        raise

    # After the commit, never before (ADR 0007 amendment): what we
    # publish must already be true. A failure does not fail the request
    # - the code is valid and committed - but the caller is told, and
    # the reconciler keeps trying.
    pub = await publish_gate(gate_id, fetchone, uploader,
                             uploader.bucket if uploader else "")

    return IssueResponse(code_id=row["code_id"].strip(),
                         plaintext=row["plaintext"], slot=row["slot"],
                         published=pub.published, publish_detail=pub.detail)


@app.delete("/admin/codes/{code_id}", dependencies=[Depends(require_admin)])
async def revoke(code_id: str) -> dict:
    row = await fetchone(
        """select c.gate_id, revoke_code(c.code_id) as done
             from access_code c where c.code_id = %s and c.revoked_at is null""",
        code_id)
    if not row or not row["done"]:
        raise HTTPException(status_code=404, detail="no such live code")

    # A revocation that reaches the database and not the bucket is the
    # worse direction of this failure: the gate goes on accepting a
    # code somebody has decided should not work.
    pub = await publish_gate(row["gate_id"], fetchone, uploader,
                             uploader.bucket if uploader else "")
    return {"revoked": code_id, "published": pub.published,
            "publish_detail": pub.detail}


@app.get("/admin/gates/{gate_id}/log", dependencies=[Depends(require_admin)])
async def read_log(gate_id: str, limit: int = 100) -> dict:
    limit = max(1, min(limit, 1000))
    rows = await fetchall(
        """select seq, device_time, caveat, received_at, event, detail,
                  code_id, code_note
             from interaction_log_readable
            where gate_id = %s
            order by seq desc
            limit %s""",
        gate_id, limit,
    )
    gaps = await fetchall(
        "select missing_from, missing_to, missing_count "
        "from interaction_gap where gate_id = %s", gate_id,
    )
    return {"events": rows, "gaps": gaps}


# ---------------------------------------------------------------------
# Publication
# ---------------------------------------------------------------------
#
# A gate reads the bucket, not this process. So "is the bucket current?"
# is the question that decides whether a guest can get in, and it needs
# an answer that does not involve reading logs.


@app.get("/admin/drift", dependencies=[Depends(require_admin)])
async def drift() -> dict:
    rows = await fetchall("select * from gate_drift order by gate_id")
    behind = [r for r in rows if r["state"] != "current"]
    return {"gates": rows, "stale": len(behind)}


@app.post("/admin/publish", dependencies=[Depends(require_admin)])
async def publish_all() -> dict:
    """Push every gate whose bucket copy is not current.

    This is the reconciler, and it is not housekeeping. A failed upload
    leaves a code that exists in the database and that the gate can
    never see; polling does not repair it, because the device is
    faithfully fetching a stale body. Run this on a timer - see
    backend/reconcile.py.
    """
    results = await reconcile(fetchone, fetchall, uploader,
                              uploader.bucket if uploader else "")
    return {
        "published": [r.gate_id for r in results if r.published],
        "failed": {r.gate_id: r.detail for r in results if not r.published},
    }


@app.post("/admin/gates/{gate_id}/publish", dependencies=[Depends(require_admin)])
async def publish_one(gate_id: str) -> dict:
    r = await publish_gate(gate_id, fetchone, uploader,
                           uploader.bucket if uploader else "")
    if not r.published and r.detail == "no such gate":
        raise HTTPException(status_code=404, detail="no such gate")
    return {"gate_id": r.gate_id, "generation": r.generation,
            "published": r.published, "detail": r.detail}


# ---------------------------------------------------------------------

@app.get("/healthz")
async def healthz() -> dict:
    await fetchone("select 1 as ok")
    return {"ok": True}
