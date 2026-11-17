"""Settings, from the environment and nowhere else."""
from __future__ import annotations

import os


class Config:
    # libpq's own variables work too; this is the one-string form that
    # docker compose and most hosts hand you.
    dsn: str = os.environ.get(
        "DATABASE_URL", "postgresql://gate@localhost:5432/gate"
    )

    # Guards the admin routes. There is no default on purpose: an API
    # that starts without one would be an API with no admin auth, and
    # that is not a thing to discover in production.
    admin_token: str | None = os.environ.get("ADMIN_TOKEN")

    # Trust X-Forwarded-For only when something in front is actually
    # setting it. Off by default, because a spoofable client address in
    # an audit trail is worse than none.
    trust_forwarded: bool = os.environ.get("TRUST_FORWARDED", "") == "1"

    pool_min: int = int(os.environ.get("POOL_MIN", "1"))
    pool_max: int = int(os.environ.get("POOL_MAX", "4"))


config = Config()
