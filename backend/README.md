# The gate backend

A small FastAPI app over Postgres. Two audiences with very different
needs: a device that polls and uploads, and an admin who issues codes.

```
backend/
  migrations/   run in filename order; compose mounts this directory
  app/          the API
  tests/        three suites, no framework, nothing to install but psql
```

## Running it

```bash
cd docker && docker compose up -d db && cd ..

pip install -r backend/requirements.txt
DATABASE_URL=... ADMIN_TOKEN=... uvicorn app.main:app --app-dir backend --reload
```

The app refuses to start without `ADMIN_TOKEN`. That is deliberate: an
API that starts anyway is an API with no admin auth, and that is not
something to discover in production.

## The security model

**No key ever leaves Postgres.** The delivery key signs the code set
going down, verifies log batches coming up, and signs the
acknowledgements. All three happen inside the database —
`gate_sync_body`, `verify_log_mac`, `gate_ack_body` — and the API
passes bytes in and gets a verdict or a signed reply out. A process
that cannot hold a key cannot log one.

That only holds if the API connects as a role without rights on
`gate_secret`:

```bash
./backend/dev_setup.sh          # prints the DATABASE_URL line to paste
```

Run that again after every `docker compose down -v` — that deletes the
volume, the volume is the whole Postgres cluster, and the role goes
with it.

`gate_app` is created by migration 0003 and has `SELECT` on four
tables, `INSERT` on `interaction_log`, and `EXECUTE` on eight
functions. Everything else it reaches through those functions, which
are `SECURITY DEFINER` with a pinned `search_path` and `EXECUTE`
revoked from `PUBLIC`.

Checked, as `api_user`:

| | |
|---|---|
| `select delivery_key from gate_secret` | permission denied |
| `update access_code …` | permission denied |
| `select purge_interaction_log(…)` | permission denied |
| the entire API test suite | passes |

**The device routes carry no credential, and need none.** Every byte in
both directions is covered by an HMAC. A caller who cannot sign gets
403 whether or not the gate exists.

## The routes

| | |
|---|---|
| `GET /gate/{id}.txt` | the signed code set; also records the poll |
| `POST /gate/{id}/log` | a signed batch, answered with a signed ack |
| `POST /admin/gates/{id}/codes` | issue; returns the plaintext **once** |
| `DELETE /admin/codes/{code_id}` | revoke |
| `GET /admin/gates/{id}/log` | the trail, in words, with its gaps |
| `GET /healthz` | |

The poll takes no query string and no headers, because the device sends
neither — it fetches the whole body every time and compares the `gen`
line to what it holds. Do not add a required parameter here.

## The tests

```bash
python3 backend/tests/verify_schema.py
python3 backend/tests/verify_interaction_log.py
DATABASE_URL=... python3 backend/tests/verify_api.py
```

The first two need only a `psql`, and will borrow the one inside the
container if there is none on the host. The third drives the real app
against the real database over an ASGI transport: the MACs are
computed with the key Postgres holds, and the bodies are the ones the
firmware would send.

All three write and delete, so two things keep them in their lane.

Every fixture lives under the gate id prefix `test-gate-`, and every
delete is scoped to it. `verify_schema.py` plants a sentinel gate
outside that prefix and checks it survives a full cleanup, so the
scoping is a tested fact rather than a claim. Each suite also clears
its own fixtures on the way out: a bench salt that outlives its run is
how one ends up signing a real gate's code set.

And they refuse to start unless the database says it is throwaway:

```sql
alter database gate set gate.disposable = 'on';
```

`dev_setup.sh` sets that. It is a database-level setting rather than an
environment variable on purpose — it travels with the database instead
of with whoever's shell is open, so pointing `DATABASE_URL` at
something real and running these by mistake stops before the first
statement.
