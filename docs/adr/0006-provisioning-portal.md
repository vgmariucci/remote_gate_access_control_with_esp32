# 0006 — Provisioning by a button inside the enclosure, never by failure

Status: accepted
Date: 2026-09-29

## Context

The device needs Wi-Fi credentials, a device salt, an HMAC key and a bot
token before it can do anything useful, and it has no screen or keyboard
worth typing them on. The USB dev console solved this on the bench, but
it is compiled out of a production image and needs a cable.

The standard answer is a SoftAP with a captive portal. The standard
implementation is also a common way these devices get owned.

## Decision

### The only way in is a deliberate hold of a button inside the enclosure

Five seconds, on a button that is only reachable with the enclosure
open. Not a short press, so closing the lid cannot arm it.

**There is deliberately no path from "Wi-Fi failed to connect" into
configuration mode.** The tempting fallback — drop into AP mode when the
network is unreachable — means anyone with a jammer can force the gate
into configuration mode from the street. `prov_tick` has no branch that
opens the AP; only `prov_button` does. A test ticks for ten simulated
minutes and asserts the AP stays shut.

A session that expires while the button is still held does not reopen:
the button must be released first, so leaning on it yields one session.

### The session is capped at five minutes, and activity does not extend it

A radio that can reconfigure the lock should be up as briefly as
possible. Five minutes is long enough to type a Wi-Fi password, and
pressing the button again is cheap. An idle timeout with a longer
absolute cap was considered and rejected as unbounded in the case that
matters: an attacker who has reached the portal keeps it alive by using
it.

### Two layers, and the outer one is WPA2

The AP itself carries a WPA2 passphrase, on a label inside the
enclosure. Without it, radio range alone reaches the login page.

Inside that, plain HTTP. HTTPS on a SoftAP means a self-signed
certificate, which trains the admin to click through a browser security
warning — a worse habit than the risk it removes, given the only
listener would already need the AP passphrase.

Five wrong logins tear the session down rather than blocking for a
while, so retrying needs physical access to the button again.
Authentication never survives a session.

### Credentials the portal will not accept

WEP is refused outright: it can be broken in minutes, and a gate that
refuses to be configured is better than one joined by a passer-by. Open
and WPA-only networks are flagged as weak but allowed — they may be the
only network available. Passphrases are checked for WPA2 lengths, with
a 64-hex-character raw PSK accepted.

Every rejection carries text the portal shows, so no failure is silent.

Credentials are **verified before they are saved**: the device joins the
network and confirms an IP before writing to NVS. Otherwise a typo is
persisted, the device reboots into a network it cannot join, and the
only recovery is the button again.

### Admin credentials are not in the repository

`main/admin_credentials.h` is gitignored, with a committed `.example`.
This repository is public, and a committed default password is how IoT
devices get taken over. The build fails loudly if the header is absent.

This is the development stand-in for the credential dongle, which will
supply them over I²C and is its own project.

### `code erase` is not exposed over the AP

It is a bench recovery tool for a chip in a known-bad state. Reachable
over radio, it is a denial of service on the property: every guest
locked out until the backend re-pushes. The portal's code page may only
touch codes for which `ac_id_is_dev()` is true.

## Consequences

An admin who walks away mid-configuration loses the session and starts
over. Accepted: the failure is visible and cheap, and the alternative
leaves a configuration radio running unattended next to a lock.

The AP passphrase is on a label inside the enclosure. Someone who has
opened the enclosure has already defeated the physical barrier — the
passphrase protects against radio-range attackers, not against someone
standing at the open box.

## Amendment, 2026-10-03: the station changes what "the radio is off" means

The decision above was written when provisioning was the only thing
that used the radio. It says the AP is the only time the gate
transmits, and that the radio is off at every other moment.

That is no longer true, and the change is deliberate.

A gate that only ever raises an AP cannot receive codes from the
backend or correct its clock. Both are required: tg_client delivers
guest codes, and SNTP is how the RTC gets set without someone typing
`time set` over USB. So once credentials have been provisioned, the
station stays associated to the house network.

### What still holds

- **The AP is still button-only.** Nothing about the station opens it,
  and there is still no path from a failed Wi-Fi join into
  configuration mode. The jammer attack the original decision rejected
  is still rejected: jamming the house network makes the gate retry,
  not open an AP.
- **The radio is still off on an unprovisioned gate.** `esp_wifi_init`
  is not called until something asks for a network, so a device that
  has never been configured transmits nothing.
- **The lock does not depend on any of it.** Validation is offline
  (ADR 0001). A gate with no network opens for a valid code.

### What changed

- A provisioned gate is associated to the house Wi-Fi continuously.
  Its exposure is now that of any other device on that network, rather
  than five minutes per configuration session.
- `esp_wifi_init` happens once and is never undone. Tearing the driver
  down and bringing it back was a source of failure modes for no gain,
  and the memory is committed the moment any network is wanted at all.
- One component, `net_link`, owns the radio. The portal asks it to
  raise and drop the AP interface. Two owners each calling
  `esp_wifi_set_mode` is how an AP disappears the instant the station
  reconnects.

### Consequence accepted

The house network is now part of the gate's attack surface. Someone on
that network can reach whatever the gate listens on. Today that is
nothing: the HTTP server runs only during a provisioning session, and
tg_client will poll outward rather than accept connections. That
property is worth keeping deliberately — **the gate should never listen
on the station interface.**
