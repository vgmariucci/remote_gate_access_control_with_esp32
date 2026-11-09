-- The gate backend, v1.
--
-- Derived from the firmware rather than from memory. Every constant
-- and every CHECK below exists because some device-side rule refuses
-- the body otherwise; the comments name which.
--
--   sync wire format   firmware/components/code_sync/include/sync_parse.h
--   telegram format    firmware/components/tg_client/include/tg_proto.h
--   table + policy     firmware/components/access_core/include/access_core.h
--   ordering rules     docs/adr/0007-amendment.md
--
-- Postgres 16. Needs pgcrypto, which every managed Postgres offers.

create extension if not exists pgcrypto;

-- ---------------------------------------------------------------------
-- Gates
-- ---------------------------------------------------------------------

create table gate (
    -- The device id the firmware was built with. It appears in the
    -- "gate <id>" line, and a device refuses a body addressed to
    -- anyone else, so one mis-pointed URL cannot load a neighbour's
    -- codes. SYNC_GATE_ID_MAX is 32 including the NUL.
    gate_id       text primary key
                  check (gate_id ~ '^[a-z0-9][a-z0-9-]{0,30}$'),

    label         text not null default '',

    -- Monotonic. The device refuses any set not strictly newer than
    -- the one it holds, which is what stops a captured body from
    -- restoring a revoked code. Never written by hand: the trigger
    -- below owns it, so that a code can never change without the
    -- generation moving in the same transaction (ADR 0007 amendment).
    generation    bigint not null default 0 check (generation >= 0),
    generation_at timestamptz not null default now(),

    -- Monotonic per gate, for the Telegram channel. tg_proto refuses a
    -- counter at or below the last one accepted, so a retry cannot be
    -- a byte-for-byte resend - it must be re-signed with a fresh one.
    tg_counter    bigint not null default 0 check (tg_counter >= 0),
    tg_chat_id    bigint,

    created_at    timestamptz not null default now()
);

comment on column gate.generation is
    'Owned by the access_code trigger. Do not UPDATE directly.';

-- Secrets live in their own table, 1:1, so that an admin query, a log
-- line or a careless SELECT * over `gate` cannot carry them. Grant on
-- this table separately; nothing but the two functions below needs it.
create table gate_secret (
    gate_id      text primary key references gate on delete cascade,

    -- sha256(code || device_salt) is what the device stores, so the
    -- backend needs the same salt the firmware has. Burned into the
    -- credential header on a production board.
    device_salt  text not null check (length(device_salt) between 16 and 128),

    -- Rotating the salt invalidates every hash computed under the old
    -- one, and because the plaintext is deliberately not stored (see
    -- access_code) those codes cannot be re-hashed - they can only be
    -- reissued. This column is how you find them afterwards.
    salt_version integer not null default 1 check (salt_version > 0),

    -- HMAC-SHA256 key, 32 bytes. Signs both channels.
    delivery_key bytea not null check (length(delivery_key) = 32),

    rotated_at   timestamptz not null default now()
);

-- ---------------------------------------------------------------------
-- Codes
-- ---------------------------------------------------------------------

create table access_code (
    -- Exactly 8 lowercase hex. ac_id_is_dev() treats anything else as
    -- a console-created code, which the sync path refuses outright, so
    -- the network can never create something that looks like a dev
    -- code. Globally unique rather than per-gate: ids are cheap and a
    -- collision across gates would be miserable to debug.
    code_id      char(8) primary key check (code_id ~ '^[0-9a-f]{8}$'),

    gate_id      text not null references gate on delete cascade,

    -- sha256(plaintext || device_salt). 32 bytes, hex-encoded on the
    -- wire.
    --
    -- THERE IS NO PLAINTEXT COLUMN, AND adding one would be a mistake.
    -- The device never stores the code; neither does this. It is
    -- generated, returned once to whoever is sending it to the guest,
    -- and forgotten. A code nobody can read is a code nobody can leak.
    code_hash    bytea not null check (length(code_hash) = 32),
    salt_version integer not null default 1 check (salt_version > 0),

    -- Mirrors AC_CODE_LEN and friends: 9 characters, exactly 6 digits,
    -- 2 letters A-D, 1 of * #. Version it, because changing the policy
    -- means reissuing rather than migrating.
    format_version smallint not null default 1,

    valid_from   timestamptz not null,
    valid_until  timestamptz not null,

    -- sync_apply validates the whole set before applying any of it, so
    -- one inverted window refuses the entire response and the gate
    -- keeps its old table. Cheaper to make it unrepresentable.
    constraint window_runs_forwards check (valid_until >= valid_from),

    -- Five slots on the device (AC_MAX_SLOTS). A sixth live code is
    -- not an error at the gate - it is silently dropped, in arrival
    -- order, and that guest simply cannot get in. Allocating a seat
    -- here makes the limit the database's problem instead.
    slot         smallint not null check (slot between 0 and 4),

    revoked_at   timestamptz,
    note         text not null default '',
    created_at   timestamptz not null default now(),
    created_by   text not null default ''
);

-- At most five un-revoked codes per gate.
--
-- The predicate cannot also test valid_until, because an index
-- predicate must be immutable and `now()` is not. So an expired code
-- keeps its seat until something revokes it: see reap_expired_codes().
create unique index access_code_one_per_slot
    on access_code (gate_id, slot)
    where revoked_at is null;

create index access_code_by_gate on access_code (gate_id)
    where revoked_at is null;

-- ---------------------------------------------------------------------
-- The generation moves with the set, or `unchanged` is a lie
-- ---------------------------------------------------------------------
--
-- ADR 0007 amendment. Bumping in a trigger rather than in application
-- code makes the rule structural: there is no way to change a gate's
-- code set without the generation moving inside the same transaction,
-- including from psql at three in the morning.
--
-- The UPDATE of `gate` also takes a row lock, which serialises
-- concurrent changes to one gate. That is the point, not a cost: it is
-- what makes generation allocation safe without an advisory lock.

create or replace function bump_generation() returns trigger
language plpgsql as $$
begin
    if tg_op = 'UPDATE' and new.gate_id is distinct from old.gate_id then
        raise exception
            'a code cannot move between gates: its hash is salted with %',
            old.gate_id;
    end if;

    update gate
       set generation = generation + 1,
           generation_at = now()
     where gate_id = coalesce(new.gate_id, old.gate_id);

    return null;
end $$;

create trigger access_code_bumps_generation
    after insert or delete
       or update of gate_id, code_hash, salt_version,
                    valid_from, valid_until, revoked_at
    on access_code
    for each row execute function bump_generation();

-- ---------------------------------------------------------------------
-- What a gate should currently hold
-- ---------------------------------------------------------------------
--
-- A future code belongs in the set: the device installs it and answers
-- AC_DENIED_NOT_YET until the window opens, which is how next week's
-- guest gets a code today. An expired one does not - it would occupy
-- one of five slots to no purpose.

create view gate_live_code as
select gate_id, code_id, code_hash, salt_version,
       valid_from, valid_until, slot
  from access_code
 where revoked_at is null
   and valid_until > now();

-- ---------------------------------------------------------------------
-- The signed body
-- ---------------------------------------------------------------------
--
-- Byte-for-byte what tools/make_sync_file.py emits, which is what
-- sync_parse.c reads:
--
--   portao-sync v1
--   gate <id>
--   gen <n>
--   code <id> <hash-hex> <from> <until>
--   end
--   mac <hex>
--
-- The MAC covers everything up to and including "end\n". Signing a
-- terminator means a truncated body cannot verify.
--
-- Built in one query inside one transaction, on purpose. Reading the
-- generation and then selecting the rows in two statements reproduces
-- the failure the ADR amendment is about: a response carrying one
-- generation and another generation's set. Here they cannot disagree.
--
-- It also keeps the delivery key out of the application process
-- entirely. FastAPI calls this and returns the text; it never holds a
-- key, so it cannot log one.

create or replace function gate_sync_body(p_gate_id text)
returns text
language plpgsql
stable
as $$
declare
    v_gen  bigint;
    v_body text;
    v_key  bytea;
begin
    select g.generation, s.delivery_key
      into v_gen, v_key
      from gate g join gate_secret s using (gate_id)
     where g.gate_id = p_gate_id;

    if not found then
        return null;
    end if;

    select 'portao-sync v1' || E'\n'
        || 'gate ' || p_gate_id || E'\n'
        || 'gen '  || v_gen || E'\n'
        || coalesce(string_agg(
               'code ' || c.code_id
                       || ' ' || encode(c.code_hash, 'hex')
                       || ' ' || floor(extract(epoch from c.valid_from))::bigint
                       || ' ' || floor(extract(epoch from c.valid_until))::bigint,
               E'\n' order by c.slot) || E'\n', '')
        || 'end' || E'\n'
      into v_body
      from gate_live_code c
     where c.gate_id = p_gate_id;

    -- convert_to() rather than a cast: the MAC is over bytes, and
    -- saying which encoding produced them is the difference between a
    -- signature that travels and one that depends on a server setting.
    return v_body
        || 'mac '
        || encode(hmac(convert_to(v_body, 'UTF8'), v_key, 'sha256'), 'hex')
        || E'\n';
end $$;

-- ---------------------------------------------------------------------
-- Issuing a code
-- ---------------------------------------------------------------------
--
-- The plaintext is generated here and returned exactly once. It is
-- never stored, and it is never a query *parameter* either - a
-- parameter can land in the statement log, a result cannot.
--
-- Format version 1, enforced to match ac_format_valid() exactly:
-- nine characters, six digits, two letters A-D, one of * #, shuffled.
-- Anything else is refused at the keypad as AC_DENIED_BAD_FORMAT.

create or replace function generate_code_v1() returns text
language plpgsql
volatile
as $$
declare
    chars text[] := '{}';
    tmp   text;
    r     bytea;
    j     int;
    n     int;
begin
    for i in 1..6 loop
        chars := chars || substr('0123456789',
                                 (get_byte(gen_random_bytes(1), 0) % 10) + 1, 1);
    end loop;
    for i in 1..2 loop
        chars := chars || substr('ABCD',
                                 (get_byte(gen_random_bytes(1), 0) % 4) + 1, 1);
    end loop;
    chars := chars || substr('*#',
                             (get_byte(gen_random_bytes(1), 0) % 2) + 1, 1);

    -- Fisher-Yates, so the special is not always in the last position.
    -- A 16-bit draw modulo at most 9 is biased by roughly 2^-16, which
    -- is far below the 2^33 the format itself is worth.
    n := array_length(chars, 1);
    for i in reverse n..2 loop
        r := gen_random_bytes(2);
        j := ((get_byte(r, 0) * 256 + get_byte(r, 1)) % i) + 1;
        tmp := chars[i];
        chars[i] := chars[j];
        chars[j] := tmp;
    end loop;

    return array_to_string(chars, '');
end $$;

create or replace function issue_code(
    p_gate_id     text,
    p_valid_from  timestamptz,
    p_valid_until timestamptz,
    p_note        text default '',
    p_created_by  text default ''
) returns table (code_id char(8), plaintext text, slot smallint)
language plpgsql
volatile
as $$
declare
    v_salt    text;
    v_saltver integer;
    v_slot    smallint;
    v_id      char(8);
    v_plain   text;
begin
    select s.device_salt, s.salt_version
      into v_salt, v_saltver
      from gate_secret s
     where s.gate_id = p_gate_id
       for update;                      -- serialise seat allocation

    if not found then
        raise exception 'no such gate: %', p_gate_id;
    end if;

    select min(g.n)::smallint into v_slot
      from generate_series(0, 4) as g(n)
     where not exists (
           select 1 from access_code c
            where c.gate_id = p_gate_id
              and c.revoked_at is null
              and c.slot = g.n);

    if v_slot is null then
        raise exception
            'gate % already holds five live codes; revoke one first',
            p_gate_id
            using hint = 'the device has five slots (AC_MAX_SLOTS)';
    end if;

    loop
        v_id := encode(gen_random_bytes(4), 'hex');
        exit when not exists (select 1 from access_code c where c.code_id = v_id);
    end loop;

    v_plain := generate_code_v1();

    insert into access_code (code_id, gate_id, code_hash, salt_version,
                             valid_from, valid_until, slot, note, created_by)
    values (v_id, p_gate_id,
            digest(convert_to(v_plain || v_salt, 'UTF8'), 'sha256'), v_saltver,
            p_valid_from, p_valid_until, v_slot, p_note, p_created_by);

    return query select v_id, v_plain, v_slot;
end $$;

create or replace function revoke_code(p_code_id char(8)) returns boolean
language sql
volatile
as $$
    update access_code
       set revoked_at = now()
     where code_id = p_code_id
       and revoked_at is null
    returning true;
$$;

-- An expired code still holds one of five seats. Run this on a timer,
-- or at the top of issue_code's caller; either way it must be
-- something, because the partial index cannot test a moving `now()`.
create or replace function reap_expired_codes() returns integer
language sql
volatile
as $$
    with gone as (
        update access_code
           set revoked_at = now()
         where revoked_at is null
           and valid_until <= now()
        returning 1
    ) select count(*)::integer from gone;
$$;

-- ---------------------------------------------------------------------
-- The Telegram channel
-- ---------------------------------------------------------------------
--
-- Allocating the counter is a database job: it must be monotonic per
-- gate under concurrency, and tg_proto refuses anything at or below
-- what the device already holds.
--
-- Call this AFTER the code is committed, never before (ADR 0007
-- amendment). What the backend pushes must already be true.

create or replace function next_tg_counter(p_gate_id text) returns bigint
language sql
volatile
as $$
    update gate
       set tg_counter = tg_counter + 1
     where gate_id = p_gate_id
    returning tg_counter;
$$;

create table tg_delivery (
    id         bigserial primary key,
    gate_id    text not null references gate on delete cascade,
    counter    bigint not null,
    op         text not null check (op in ('add', 'rev')),
    code_id    char(8),
    sent_at    timestamptz not null default now(),
    ok         boolean not null,
    detail     text not null default '',

    unique (gate_id, counter)
);

-- ---------------------------------------------------------------------
-- Is the gate alive?
-- ---------------------------------------------------------------------
--
-- The device reports nothing. Its polls are the only evidence it still
-- exists, which makes this the one piece of monitoring the system
-- would otherwise lack - and ADR 0007 exists because silent failure is
-- the enemy. One row per gate, updated in place.

create table gate_seen (
    gate_id            text primary key references gate on delete cascade,
    last_poll_at       timestamptz not null,
    last_generation    bigint not null,
    poll_count         bigint not null default 1,
    remote_addr        inet,
    user_agent         text not null default ''
);

create or replace function note_poll(
    p_gate_id text, p_generation bigint,
    p_addr inet default null, p_ua text default ''
) returns void
language sql
volatile
as $$
    insert into gate_seen (gate_id, last_poll_at, last_generation,
                           remote_addr, user_agent)
    values (p_gate_id, now(), p_generation, p_addr, p_ua)
    on conflict (gate_id) do update
       set last_poll_at    = now(),
           last_generation = excluded.last_generation,
           poll_count      = gate_seen.poll_count + 1,
           remote_addr     = excluded.remote_addr,
           user_agent      = excluded.user_agent;
$$;
