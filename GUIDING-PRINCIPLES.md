# MeshPigeon Firmware — Guiding Principles

> **MeshPigeon exists so a stranger to mesh radio can install, connect, and
> message in minutes — offline, forever.**
>
> 1. Protocol in the app, never the firmware. Firmware stays dumb and durable.
> 2. Hide mechanics; expose outcomes. Users see "heard ✓", not "flood route ACK".
> 3. Offline-first: the app must always be useful with no radio and no internet.
> 4. No artificial limits: history, contacts, channels live in the app DB.
> 5. Layers stay separated (UI / domain / protocol / transport) and tested;
>    the app's `:core-protocol` never imports Android.
> 6. Minimal diffs; match existing style; every change maps to a stated need.
> 7. Accessibility is a requirement: ≥48 dp targets, screen-reader labels,
>    large type support.
> 8. When features conflict, order: newcomer experience → reliability →
>    enthusiast features.

## Firmware addendum

> The radio stores packets it cannot read and persists its settings; it never
> transmits on its own initiative. If a proposed change gives the firmware
> opinions about repeating, message content, keys, or identity — stop; it
> belongs in the app.

Concrete consequences, enforced in review:

- The firmware keeps **raw packet bytes only** (no payload parsing beyond a
  byte count), which is what maximizes retained capacity.
- No dedup table, no repeat mode, no ACK/retry logic, no identity, no keys.
  These concepts do not exist in this codebase.
- The only opinionated thing the firmware holds is its persisted **radio
  settings** (frequency, bandwidth, spreading factor, coding rate, power,
  region preset id).
- The in-app "repeater" is an app feature: while connected, the app decides
  what to forward via `SEND_PACKET`. The radio forwards nothing on its own.
