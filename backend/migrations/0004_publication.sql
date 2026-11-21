-- 0004 - what is actually live in the bucket.
--
-- ADR 0007 made the poll the correctness mechanism: one successful
-- response and the gate is right, whatever happened before it. That
-- argument only holds if the thing being polled is up. Serving it from
-- the API puts a Python process on the gate's correctness path, where
-- it does not belong.
--
-- So the signed body is written to object storage and the device polls
-- that. The API becomes an admin tool that can be down for a day
-- without a guest noticing.
--
-- The cost is a new way to be silently wrong: a code committed here
-- that never reached the bucket. The gate cannot see it, the database
-- says it exists, and nothing complains. This migration exists to make
-- that state visible and fixable.

create table gate_publication (
    gate_id       text primary key references gate on delete cascade,

    -- The generation of the body currently in the bucket. Written only
    -- after a PUT succeeds: recording it first would mean a failed
    -- upload looked published, which is the one outcome nothing later
    -- could detect.
    generation    bigint not null check (generation >= 0),
    published_at  timestamptz not null default now(),

    -- Where it went, recorded rather than assumed, so that changing the
    -- bucket does not silently leave half the gates pointed at the old
    -- one.
    bucket        text not null,
    key           text not null,
    etag          text not null default '',

    -- Last failure, kept until the next success. A publisher that
    -- retries forever and says nothing is how a stale bucket survives a
    -- weekend.
    last_error    text not null default '',
    last_error_at timestamptz
);

-- The whole point of the table: one query that answers "is any gate
-- showing the wrong code set?"
create view gate_drift as
select g.gate_id,
       g.generation                     as db_generation,
       p.generation                     as live_generation,
       p.published_at,
       p.last_error,
       p.last_error_at,
       case
            when p.gate_id is null                 then 'never published'
            when p.generation < g.generation       then 'behind'
            when p.generation > g.generation       then 'ahead of the database'
            else                                        'current'
       end as state
  from gate g
  left join gate_publication p using (gate_id);

-- Recorded only on success, and only forwards.
--
-- Two publishers racing can finish out of order - the second one to
-- start can be the first to land. Refusing to record a lower
-- generation keeps this table honest about the newest body known to
-- have been written.
--
-- An out-of-order PUT cannot hurt the gate either way: the device
-- refuses any generation not strictly newer than the one it holds
-- (sync_logic, SYNC_STALE), so the worst case is one wasted poll, and
-- the reconciler puts the current body back.
create or replace function note_published(
    p_gate_id    text,
    p_generation bigint,
    p_bucket     text,
    p_key        text,
    p_etag       text default ''
) returns boolean
language plpgsql
volatile
as $$
begin
    insert into gate_publication (gate_id, generation, bucket, key, etag)
    values (p_gate_id, p_generation, p_bucket, p_key, p_etag)
    on conflict (gate_id) do update
       set generation    = excluded.generation,
           published_at  = now(),
           bucket        = excluded.bucket,
           key           = excluded.key,
           etag          = excluded.etag,
           last_error    = '',
           last_error_at = null
     where gate_publication.generation <= excluded.generation;

    return found;
end $$;

create or replace function note_publish_failed(
    p_gate_id text,
    p_error   text
) returns void
language plpgsql
volatile
as $$
begin
    -- A failure before any success has nothing to attach itself to;
    -- gate_drift reports that as "never published", which is louder
    -- than a row claiming generation 0.
    update gate_publication
       set last_error    = left(p_error, 500),
           last_error_at = now()
     where gate_id = p_gate_id;
end $$;

grant select on gate_publication, gate_drift to gate_app;

do $$
declare
    f text;
    sigs text[] := array[
        'note_published(text, bigint, text, text, text)',
        'note_publish_failed(text, text)'
    ];
begin
    foreach f in array sigs loop
        execute format('revoke execute on function %s from public', f);
        execute format('grant  execute on function %s to gate_app', f);
        execute format(
            'alter function %s security definer set search_path = pg_catalog, public',
            f);
    end loop;
end $$;
