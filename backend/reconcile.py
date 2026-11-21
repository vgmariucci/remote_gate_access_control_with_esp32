#!/usr/bin/env python3
"""Put the bucket back in step with the database.

Run on a timer. This is not tidy-up: a failed upload leaves a code that
exists in the database and that the gate can never see, and no amount
of polling repairs it - the device is faithfully fetching a stale body.
Something has to notice, and this is it.

    */5 * * * * cd /srv/portao && . docker/.env && \
                python3 backend/reconcile.py >> /var/log/portao-publish.log 2>&1

Exit status is 0 when every gate is current afterwards, 1 when any gate
is still behind - so a cron wrapper or a monitor can alert on it.
"""
from __future__ import annotations

import asyncio
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from app import main as api  # noqa: E402
from app.publish import S3Uploader, reconcile  # noqa: E402

logging.basicConfig(
    level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s %(message)s"
)
log = logging.getLogger("gate.reconcile")


async def run() -> int:
    uploader = S3Uploader.from_env()
    if uploader is None:
        log.error("S3_BUCKET is not set: there is nowhere to publish to")
        return 2

    async with api.app.router.lifespan_context(api.app):
        before = await api.fetchall(
            "select gate_id, state from gate_drift where state <> 'current'")
        if not before:
            log.info("every gate is current")
            return 0

        log.info("%d gate(s) not current: %s", len(before),
                 ", ".join(f"{r['gate_id']} ({r['state']})" for r in before))

        results = await reconcile(api.fetchone, api.fetchall,
                                  uploader, uploader.bucket)

        failed = [r for r in results if not r.published]
        for r in results:
            if r.published:
                log.info("published %s generation %d", r.gate_id, r.generation)
            else:
                log.error("still behind: %s - %s", r.gate_id, r.detail)

        return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(run()))
