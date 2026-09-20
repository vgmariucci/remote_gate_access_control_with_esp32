# 0003 — Fail-secure solenoid, pulsed, with a mechanical override

Status: accepted
Date: 2026-09-19

## Context

The door is not a motorised gate. It is a standard door with a 12 V
electric lock (HDL, with a mechanical cylinder in the same body). Two
lock technologies were available:

- **Fail-secure** solenoid strike (*fecho elétrico*): locked when
  unpowered, energised briefly to release.
- **Fail-safe** electromagnet (*eletroímã*): unlocked when unpowered,
  energised continuously to hold.

The property is a short-term rental, unattended, with guests arriving at
arbitrary hours and no one on site to intervene.

## Decision

Fail-secure, driven as a pulse, with a mechanical cylinder as the
override.

A power cut therefore leaves the door **locked**, not open. That is the
correct failure for an unattended rental: an electromagnet that releases
on power loss turns any outage — or any cut wire — into an open door.

The cost of that choice is that a dead system locks the guest *out*
rather than letting them in. We accept it and solve it with three
independent layers rather than by inverting the lock:

1. **Offline validation** (ADR 0001): loss of internet is a non-event.
2. **Battery backup**: a 12 V 7 Ah SLA behind a no-break module. The
   quiescent load is essentially the ESP32 at 60–90 mA from 12 V, giving
   35–45 hours of real autonomy. The solenoid is irrelevant to the
   energy budget: ten openings a day at 1.5 A for 1 s is about 4 mAh.
3. **Mechanical cylinder**, in the same lock body, plus a key-switch
   wired in parallel with the driver output. The electronics can fail
   completely without stranding anyone.

## Consequences

**The pulse must be capped in firmware.** These coils draw 1.5–2 A
inrush and are rated for momentary energisation only; holding one on
burns it. `lock_driver` caps the pulse at 800 ms–1 s with a ceiling no
code path can exceed, backed by the task watchdog, plus a 3 A fuse on
the lock rail. This is the single most common way these builds fail in
the field.

**The rails must be separated.** Solenoid inrush browning out the ESP32
mid-pulse presents as random reboots that take a week to diagnose.
Separate buck for the logic, a series Schottky so the logic rail cannot
sag back into the lock rail, 2200 µF bulk on the lock side, 470 µF on
the logic side, and a flyback diode directly across the coil terminals.
Drive through an optocoupler into a logic-level MOSFET, never straight
off a GPIO.

**Brownout detection is enabled** (`CONFIG_ESP_BROWNOUT_DET_LVL_SEL_7`).
It turns a supply sag into a clean, logged reset instead of silent
memory corruption, and it tells us honestly whether the bulk capacitors
are sized correctly.

**The key is not given to guests.** It lives in a coded keybox or with a
caretaker. Handing it out would defeat the expiring-code model entirely.
