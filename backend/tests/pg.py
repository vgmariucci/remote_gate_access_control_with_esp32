"""Talking to the development database, from wherever it happens to be.

Shared by the verify_*.py suites. No dependencies beyond the standard
library and a psql - the point is that checking the schema needs
nothing installed that running the project does not already need.
"""
import os
import shutil
import subprocess
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

FAILURES: list[str] = []


def _run(sql: str) -> subprocess.CompletedProcess:
    # -X so a developer's ~/.psqlrc cannot change the output format out
    # from under the parsing that follows.
    return subprocess.run(PSQL + ["-X", "-tAq", "-v", "ON_ERROR_STOP=1", "-c", sql],
                          env=ENV, capture_output=True, text=True)


def q(sql: str) -> str:
    r = _run(sql)
    if r.returncode != 0:
        raise RuntimeError(r.stderr.strip())
    return r.stdout.rstrip("\n")


def q_fails(sql: str) -> str:
    """Run something that must not be allowed, and return the complaint."""
    r = _run(sql)
    if r.returncode == 0:
        raise RuntimeError(f"expected a failure, got success: {sql}")
    return r.stderr.strip()


def check(name: str, ok: bool, detail: str = "") -> None:
    print(f"{'ok  ' if ok else 'FAIL'}  {name}"
          + (f"\n        {detail}" if detail and not ok else ""))
    if not ok:
        FAILURES.append(name)


def banner() -> None:
    print(f"via {TRANSPORT}\n")


def summary() -> int:
    print()
    if FAILURES:
        print(f"{len(FAILURES)} failure(s): {', '.join(FAILURES)}")
        return 1
    print("all checks passed")
    return 0
