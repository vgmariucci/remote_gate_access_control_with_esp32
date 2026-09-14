# ADR 0001: The gate validates offline, the cloud only syncs

Status: accepted
Date: 2026-09-13

## Context

The controller sits at a gate on a residential internet link. Guests
arrive at arbitrary hours, frequently late at night, often without a
working local SIM.

An obvious design has the ESP32 ask a backend whether a typed code is
valid. It is simpler to write and it centralises the decision.

## Decision

The device holds a table of up to 16 `{id, sha256(code||salt), valid_from,
valid_until}` entries in encrypted NVS and decides locally. The network is
used only to add, replace and remove entries, and to upload the attempt
log.

## Consequences

Good: a dropped link, a dead router or a Telegram outage does not lock a
paying guest out of the property. Latency at the keypad is a few
milliseconds rather than a round trip.

Bad: revocation is no longer instant. A cancelled booking's code keeps
working at the gate until the device acknowledges the `revoke` command, so
the UI must show sync state honestly rather than implying that a database
update reached the hardware.

Bad: the device needs trustworthy time to enforce `valid_until`, which is
what forces the DS3231 and the fail-closed policy in ADR 0002.
