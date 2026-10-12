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