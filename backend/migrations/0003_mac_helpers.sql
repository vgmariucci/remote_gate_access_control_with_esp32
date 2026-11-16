-- 0003 - signing and verifying, without the key leaving Postgres.
--
-- gate_sync_body() (0001) already builds the downward body and signs it
-- here, so the application never holds a delivery key and therefore
-- cannot log one. The log channel (ADR 0008) needs the same key twice
-- more - to check an upload's MAC, and to sign the acknowledgement -
-- and doing either in Python would undo that property for the sake of
-- two functions.
--
-- So the application passes bytes in and gets a verdict or a signed
-- reply out. It never sees a key.

-- ---------------------------------------------------------------------
-- Verifying an upload
-- ---------------------------------------------------------------------
--
-- The caller hands over the signed region exactly as it arrived, and
-- the 64 hex characters that followed it. Everything else about the
-- body stays unparsed until this returns true: authenticate first,
-- believe the fields second, which is the order tg_proto uses and for
-- the same reason - a forged message must not be able to steer the
-- parser with its contents.

create or replace function verify_log_mac(
    p_gate_id text,
    p_signed  text,
    p_mac_hex text
) returns boolean
language plpgsql
volatile                 -- gen_random_bytes below
as $$
declare
    v_key  bytea;
    v_ours bytea;
    v_mask bytea;
begin
    -- Malformed hex would make decode() raise, which is an answer of a
    -- different kind than "no".
    if p_mac_hex is null or p_mac_hex !~ '^[0-9a-f]{64}$' then
        return false;
    end if;

    select delivery_key into v_key from gate_secret where gate_id = p_gate_id;
    if not found then
        return false;
    end if;

    v_ours := hmac(convert_to(p_signed, 'UTF8'), v_key, 'sha256');

    -- Compare under a fresh random key rather than byte for byte.
    -- Postgres's bytea = is not constant time, and while the jitter of
    -- a network makes that hard to exploit, "hard to exploit" is a
    -- worse property than "carries no timing signal at all", and this
    -- costs two hashes.
    v_mask := gen_random_bytes(32);
    return hmac(v_ours, v_mask, 'sha256')
         = hmac(decode(p_mac_hex, 'hex'), v_mask, 'sha256');
end $$;

-- ---------------------------------------------------------------------
-- Signing the acknowledgement
-- ---------------------------------------------------------------------
--
-- ADR 0008: from the device's side an ack is a delete primitive - it
-- is the instruction to forget events. An unsigned one lets anyone who
-- can answer the device tell it to discard the record of what just
-- happened at the door. TLS stops an on-path attacker; this makes it
-- untrue regardless of who the device ended up talking to.
--
--   portao-ack v1
--   gate <device-id>
--   acked <seq>
--   end
--   mac <hex>
--
-- The MAC covers everything up to and including "end\n", as in both
-- other formats. The first line is the domain separator: the same key
-- signs "portao-sync v1" bodies going down and verifies "portao-log
-- v1" batches coming up, and because the magic sits inside the signed
-- region no message of one kind can be presented as another.

create or replace function gate_ack_body(
    p_gate_id text,
    p_acked   bigint
) returns text
language plpgsql
stable
as $$
declare
    v_key  bytea;
    v_body text;
begin
    select delivery_key into v_key from gate_secret where gate_id = p_gate_id;
    if not found then
        return null;
    end if;

    v_body := 'portao-ack v1' || E'\n'
           || 'gate '  || p_gate_id || E'\n'
           || 'acked ' || p_acked   || E'\n'
           || 'end'    || E'\n';

    return v_body
        || 'mac '
        || encode(hmac(convert_to(v_body, 'UTF8'), v_key, 'sha256'), 'hex')
        || E'\n';
end $$;

-- ---------------------------------------------------------------------
-- What the application may call, and nothing else
-- ---------------------------------------------------------------------
--
-- gate_app holds no rights on gate_secret, access_code, gate_seen or
-- interaction_upload. These functions are SECURITY DEFINER, so they run
-- as their owner and the application reaches those tables only through
-- the door each one opens. That is the whole access model: a short list
-- of verbs rather than a table grant.
--
-- Three things make that safe rather than a hole:
--   * EXECUTE is revoked from PUBLIC first - Postgres grants it by
--     default, and a SECURITY DEFINER function the world may call is
--     exactly the shape of a privilege-escalation bug.
--   * search_path is pinned, so nothing can be resolved out of a
--     schema the caller controls.
--   * none of them take a table or column name as an argument.

grant usage on schema public to gate_app;

do $$
declare
    f text;
    sigs text[] := array[
        'verify_log_mac(text, text, text)',
        'gate_ack_body(text, bigint)',
        'gate_sync_body(text)',
        'ingest_log_batch(text, jsonb, bigint, inet)',
        'note_poll(text, bigint, inet, text)',
        'issue_code(text, timestamptz, timestamptz, text, text)',
        'revoke_code(char)',
        'next_tg_counter(text)'
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

-- purge_interaction_log is deliberately absent from that list. Deleting
-- rows is a maintenance act, not something the API should be able to be
-- talked into.
revoke execute on function purge_interaction_log(interval) from public;
