"""Putting the signed body where the gate can reach it.

The device polls object storage, not this process (ADR 0007: the poll
is the correctness mechanism, so it should run on the most boring,
most available thing we own). What is here is the part that keeps the
bucket in step with the database, and says so loudly when it is not.

The uploader is injected rather than imported. Every interesting
failure here is a storage failure, and a test that cannot make the
upload fail on demand is not testing anything - same reason the
firmware injects its MAC function.
"""
from __future__ import annotations

import logging
import os
from dataclasses import dataclass
from typing import Callable, Protocol

log = logging.getLogger("gate.publish")

# The device's URL ends in this, and the bucket policy is scoped to
# `gate-*.txt`. Changing the shape here means changing both.
KEY_TEMPLATE = "gate-{gate_id}.txt"


class Uploader(Protocol):
    def put(self, key: str, body: bytes) -> str:
        """Store `body` at `key`. Returns an ETag, or raises."""


@dataclass(frozen=True)
class Result:
    gate_id: str
    generation: int
    published: bool
    detail: str = ""


class S3Uploader:
    """The real one. Constructed from the environment, lazily.

    Nothing imports boto3 until a publish is actually attempted, so the
    tests and anyone running without AWS credentials are unaffected.
    """

    def __init__(self, bucket: str, prefix: str = "", region: str | None = None):
        self.bucket = bucket
        self.prefix = prefix
        self.region = region
        self._client = None

    @classmethod
    def from_env(cls) -> "S3Uploader | None":
        bucket = os.environ.get("S3_BUCKET", "").strip()
        if not bucket:
            return None
        return cls(
            bucket=bucket,
            prefix=os.environ.get("S3_PREFIX", "").strip().lstrip("/"),
            region=os.environ.get("AWS_REGION") or None,
        )

    def client(self):
        if self._client is None:
            import boto3  # noqa: PLC0415 - deliberately lazy

            self._client = boto3.client("s3", region_name=self.region)
        return self._client

    def full_key(self, key: str) -> str:
        return f"{self.prefix}/{key}" if self.prefix else key

    def put(self, key: str, body: bytes) -> str:
        r = self.client().put_object(
            Bucket=self.bucket,
            Key=self.full_key(key),
            Body=body,
            ContentType="text/plain; charset=utf-8",
            # The device polls every fifteen minutes and a revocation
            # is only as fast as the freshest copy anyone will serve.
            # S3 itself does not cache, but anything put in front of it
            # will, given the chance.
            CacheControl="no-cache, max-age=0",
        )
        return (r.get("ETag") or "").strip('"')


async def publish_gate(
    gate_id: str,
    fetchone: Callable,
    uploader: Uploader | None,
    bucket_name: str,
) -> Result:
    """Write one gate's current signed body to storage.

    Ordering is the whole of it: build the body, upload it, and only
    then record what was published. Recording first would mean a failed
    upload looked successful, which is the single outcome nothing
    downstream could ever detect.

    A failure here does not raise. The code it was carrying is already
    committed and valid; what has failed is delivery, and the honest
    thing is to record that and let the reconciler keep trying.
    """
    row = await fetchone("select gate_sync_body(%s) as body", gate_id)
    if row is None or row["body"] is None:
        return Result(gate_id, 0, False, "no such gate")

    body: str = row["body"]
    generation = _generation_of(body)

    if uploader is None:
        return Result(gate_id, generation, False, "no bucket configured")

    key = KEY_TEMPLATE.format(gate_id=gate_id)
    try:
        etag = uploader.put(key, body.encode("utf-8"))
    except Exception as e:  # noqa: BLE001 - every storage failure is the same here
        detail = f"{type(e).__name__}: {e}"[:500]
        log.warning("publish failed for %s: %s", gate_id, detail)
        await fetchone("select note_publish_failed(%s, %s)", gate_id, detail)
        return Result(gate_id, generation, False, detail)

    await fetchone(
        "select note_published(%s, %s, %s, %s, %s) as recorded",
        gate_id, generation, bucket_name, key, etag,
    )
    log.info("published %s generation %d", gate_id, generation)
    return Result(gate_id, generation, True)


async def reconcile(fetchone, fetchall, uploader, bucket_name) -> list[Result]:
    """Publish every gate whose bucket copy is not current.

    This is not a tidy-up, it is the mechanism. A failed upload leaves a
    code that exists in the database and cannot be seen by the gate,
    and no amount of polling fixes that - the device would be fetching
    a stale body faithfully, forever. Something has to run this on a
    timer.
    """
    rows = await fetchall(
        "select gate_id from gate_drift where state <> 'current' order by gate_id"
    )
    out = []
    for r in rows:
        out.append(await publish_gate(r["gate_id"], fetchone, uploader, bucket_name))
    return out


def _generation_of(body: str) -> int:
    for line in body.split("\n"):
        if line.startswith("gen "):
            return int(line[4:])
    return 0
