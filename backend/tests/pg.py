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

# Every fixture these suites create lives under this prefix, and every
# row they delete is scoped to it. A real gate in the same database is
# never touched - which is not a thing to take on trust, so
# verify_schema.py plants a sentinel and checks it survives.
TEST_PREFIX = "test-gate-"
GATE = f"{TEST_PREFIX}01"
OTHER_GATE = f"{TEST_PREFIX}02"


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


def require_disposable() -> None:
    """Refuse to run against a database nobody has said is throwaway.

    These suites write and delete. Scoping every statement to
    TEST_PREFIX means a real gate survives even so, but "survives a
    destructive test run" is a weaker promise than "the destructive
    test run does not start".

    The marker is a database-level setting rather than an environment
    variable, because it travels with the database instead of with
    whoever's shell happens to be open:

        alter database gate set gate.disposable = 'on';

    backend/dev_setup.sh does that for the development container. A
    production database simply never has it, so pointing DATABASE_URL
    at one and running these by mistake stops here.
    """
    marker = q("select coalesce(current_setting('gate.disposable', true), '');")
    if marker != "on":
        raise SystemExit(
            "refusing to run: this database is not marked disposable.\n"
            "  These suites create and delete rows. To mark a development\n"
            "  database:\n"
            "      alter database <db> set gate.disposable = 'on';\n"
            "  or run backend/dev_setup.sh, which does it along with the\n"
            "  api_user role."
        )


def cleanup() -> None:
    """Remove this suite's fixtures, and only this suite's.

    interaction_log and interaction_upload reference gate without a
    cascade - deliberately, so that removing a gate cannot quietly take
    its history - so they go first.
    """
    q(f"""set session gate.retention = 'on';
          delete from interaction_log    where gate_id like '{TEST_PREFIX}%';
          delete from interaction_upload where gate_id like '{TEST_PREFIX}%';
          delete from tg_delivery        where gate_id like '{TEST_PREFIX}%';
          delete from gate               where gate_id like '{TEST_PREFIX}%';""")


def banner() -> None:
    print(f"via {TRANSPORT}\n")
    require_disposable()
    cleanup()


def summary() -> int:
    # Leave nothing behind. A fixture that outlives its run is how a
    # bench salt ends up signing a real gate's code set.
    cleanup()
    print()
    if FAILURES:
        print(f"{len(FAILURES)} failure(s): {', '.join(FAILURES)}")
        return 1
    print("all checks passed")
    return 0
