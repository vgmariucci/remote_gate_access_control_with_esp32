-- 0002 - the interaction log (ADR 0008).
--
-- The gate records fixed-size events in its EEPROM and uploads them
-- signed, at least once, keeping each until this side acknowledges it
-- by sequence number.
--
-- The device knows code_id and never a guest. Identity lives here, in
-- one place, which is what makes the log pseudonymous by construction
-- and "forget this guest" a single deletion.

-- ---------------------------------------------------------------------
-- The log
-- ---------------------------------------------------------------------

create table interaction_log (
    -- No ON DELETE CASCADE. Removing a gate must not silently take its
    -- history with it; if you really mean to, deal with the log first.
    gate_id       text   not null references gate,
    seq           bigint not null check (seq >= 0),

    -- Two timestamps, never one. A gate that boots before SNTP has a
    -- plausible-looking wrong clock, and a log that mixes trusted and
    -- untrusted times silently is worse than one that says which is
    -- which. device_time is what the gate believed; received_at is the
    -- only time this side can vouch for.
    device_time   timestamptz not null,
    clock_trusted boolean     not null,
    received_at   timestamptz not null default now(),

    event         text not null check (event in (
                      'boot',          -- result = esp_reset_reason_t
                      'granted',
                      'denied',        -- result = ac_result_t
                      'lockout',
                      'session_open',  -- console or provisioning portal
                      'session_close')),

    -- One byte on the device, and its meaning is given by `event`:
    -- ac_result_t for a denial, esp_reset_reason_t for a boot, unused
    -- otherwise. interaction_log_readable below renders it.
    result        smallint,

    -- Deliberately NOT a foreign key. An audit record has to survive
    -- the deletion of the thing it describes, and a denial against an
    -- unknown hash has nothing to reference at all.
    code_id       char(8) check (code_id ~ '^[0-9a-f]{8}$'),

    flags         smallint not null default 0,

    -- Makes at-least-once delivery idempotent for free.
    primary key (gate_id, seq),

    -- A denial that does not say why is not worth storing.
    constraint denial_carries_a_reason
        check (event <> 'denied' or result is not null)
);

create index interaction_log_by_time on interaction_log (gate_id, device_time desc);
create index interaction_log_by_code on interaction_log (code_id)
    where code_id is not null;

-- ---------------------------------------------------------------------
-- Append-only, and meant
-- ---------------------------------------------------------------------
--
-- Roles alone are not enough here: the owner of a table can always
-- write to it, and the owner is whoever is holding psql at three in
-- the morning. So the rule is a trigger, which stops an accident from
-- any role, and the grants below stop the application from being
-- talked into one.
--
-- Deletion is permitted only inside purge_interaction_log(), which
-- sets a transaction-local flag. Dropping the trigger is of course
-- possible - but it is then a deliberate act, which is all a log can
-- ever ask for.

create or replace function interaction_log_append_only() returns trigger
language plpgsql as $$
begin
    if tg_op = 'UPDATE' then
        raise exception 'interaction_log is append-only (gate %, seq %)',
            old.gate_id, old.seq;
    end if;

    if coalesce(current_setting('gate.retention', true), '') <> 'on' then
        raise exception
            'interaction_log rows are removed only by purge_interaction_log()';
    end if;

    return old;
end $$;

create trigger interaction_log_is_append_only
    before update or delete on interaction_log
    for each row execute function interaction_log_append_only();

-- The application inserts and reads; it never removes. Grant this to
-- whatever login role the API uses, and do not let the API connect as
-- the owner of these tables.
--
-- Guarded, because roles live in the cluster rather than in this
-- database: a second `docker compose up` against a surviving volume,
-- or a second database in the same cluster, would otherwise fail here.
do $$
begin
    if not exists (select 1 from pg_roles where rolname = 'gate_app') then
        create role gate_app nologin;
    end if;
end $$;

grant select, insert on interaction_log to gate_app;
grant select on gate, access_code, gate_seen to gate_app;

-- ---------------------------------------------------------------------
-- Uploads, and the holes between them
-- ---------------------------------------------------------------------

create table interaction_upload (
    id          bigserial primary key,
    gate_id     text not null references gate,
    received_at timestamptz not null default now(),
    from_seq    bigint not null,
    to_seq      bigint not null,
    rows_stored integer not null,

    -- The device's lifetime count of events overwritten in the ring
    -- before they could be uploaded. Cumulative rather than
    -- since-last-batch, so that a retried upload reports the same
    -- number instead of counting the same losses twice.
    dropped     bigint not null check (dropped >= 0),

    remote_addr inet,

    check (to_seq >= from_seq)
);

create index interaction_upload_by_gate
    on interaction_upload (gate_id, received_at desc);

-- Both mechanisms, because they answer different questions: a gap in
-- the sequence says *something is missing*, and the dropped counter
-- says *it is missing permanently, stop waiting for it*.
create view interaction_gap as
select gate_id,
       prev_seq + 1 as missing_from,
       seq - 1      as missing_to,
       seq - prev_seq - 1 as missing_count
  from (select gate_id, seq,
               lag(seq) over (partition by gate_id order by seq) as prev_seq
          from interaction_log) t
 where prev_seq is not null
   and seq - prev_seq > 1;

-- ---------------------------------------------------------------------
-- Ingest
-- ---------------------------------------------------------------------
--
-- One transaction, idempotent, and it returns the acknowledgement the
-- device is waiting for: the highest sequence number now durably
-- stored for that gate.
--
-- The caller has already verified the MAC. That order matters and is
-- the same one tg_proto uses: authenticate the whole body, then
-- believe its fields.
--
-- Events arrive as JSON because the application has to parse the text
-- format anyway in order to check the signature; re-parsing it here
-- would be a second implementation of the same grammar.

create or replace function ingest_log_batch(
    p_gate_id text,
    p_events  jsonb,              -- [{seq, ts, event, result, code_id, clock_trusted, flags}, ...]
    p_dropped bigint default 0,
    p_addr    inet   default null
) returns bigint
language plpgsql
volatile
as $$
declare
    v_stored integer;
    v_from   bigint;
    v_to     bigint;
    v_ack    bigint;
begin
    if not exists (select 1 from gate where gate_id = p_gate_id) then
        raise exception 'no such gate: %', p_gate_id;
    end if;

    if jsonb_typeof(p_events) <> 'array' then
        raise exception 'p_events must be a JSON array';
    end if;

    with incoming as (
        select (e->>'seq')::bigint                         as seq,
               to_timestamp((e->>'ts')::bigint)            as device_time,
               coalesce((e->>'clock_trusted')::boolean, false) as clock_trusted,
               e->>'event'                                 as event,
               nullif(e->>'result', '')::smallint          as result,
               nullif(nullif(e->>'code_id', ''), '-')      as code_id,
               coalesce((e->>'flags')::smallint, 0)        as flags
          from jsonb_array_elements(p_events) as e
    ), put as (
        insert into interaction_log (gate_id, seq, device_time, clock_trusted,
                                     event, result, code_id, flags)
        select p_gate_id, seq, device_time, clock_trusted,
               event, result, code_id, flags
          from incoming
        -- A duplicate is free: the device re-sends until acked, and a
        -- crash between storing and acking must cost a repeat rather
        -- than a loss.
        on conflict (gate_id, seq) do nothing
        returning 1
    )
    select (select count(*) from put),
           (select min(seq) from incoming),
           (select max(seq) from incoming)
      into v_stored, v_from, v_to;

    if v_to is not null then
        insert into interaction_upload (gate_id, from_seq, to_seq,
                                        rows_stored, dropped, remote_addr)
        values (p_gate_id, v_from, v_to, v_stored, p_dropped, p_addr);
    end if;

    -- The ack is over everything held for this gate, not just this
    -- batch, so that a replayed batch still returns something sane.
    select coalesce(max(seq), -1) into v_ack
      from interaction_log where gate_id = p_gate_id;

    return v_ack;
end $$;

-- ---------------------------------------------------------------------
-- Retention
-- ---------------------------------------------------------------------
--
-- An access record tied to an identifiable guest is personal data, and
-- this runs in Brazil under the LGPD. Keeping everything forever is a
-- decision too, just not a considered one.
--
-- set_config(..., true) is transaction-local, so the permission to
-- delete cannot leak out of this call into the rest of a session.

create or replace function purge_interaction_log(p_older_than interval)
returns integer
language plpgsql
volatile
as $$
declare
    n integer;
begin
    if p_older_than < interval '1 day' then
        raise exception 'refusing to purge anything younger than a day (%)',
            p_older_than;
    end if;

    perform set_config('gate.retention', 'on', true);
    delete from interaction_log where received_at < now() - p_older_than;
    get diagnostics n = row_count;
    return n;
end $$;

-- ---------------------------------------------------------------------
-- For reading
-- ---------------------------------------------------------------------
--
-- Same principle as the text wire format: something a person can make
-- sense of at three in the morning without a lookup table.

create view interaction_log_readable as
select l.gate_id,
       l.seq,
       l.device_time,
       case when l.clock_trusted then null else 'clock untrusted' end as caveat,
       l.received_at,
       l.event,
       case l.event
            when 'denied' then
                case l.result
                    when 1 then 'unknown code'
                    when 2 then 'before its window'
                    when 3 then 'after its window'
                    when 4 then 'locked out'
                    when 5 then 'no trusted clock'
                    when 6 then 'bad format'
                    else 'ac_result_t ' || l.result
                end
            when 'boot' then
                'reset: ' || case l.result
                    when 1 then 'power-on'      when 2 then 'external'
                    when 3 then 'software'      when 4 then 'panic'
                    when 5 then 'interrupt watchdog'
                    when 6 then 'task watchdog' when 7 then 'another watchdog'
                    when 8 then 'deep sleep'    when 9 then 'brownout'
                    when 10 then 'SDIO'         when 11 then 'USB peripheral'
                    when 12 then 'JTAG'         when 13 then 'efuse error'
                    when 14 then 'power glitch' when 15 then 'CPU lockup'
                    else 'unknown' end
            else null
       end as detail,
       l.code_id,
       c.note as code_note
  from interaction_log l
  left join access_code c on c.code_id = l.code_id;

grant select on interaction_log_readable, interaction_gap to gate_app;
