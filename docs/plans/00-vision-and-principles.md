# 00 — Vision & Guiding Principles

## Vision

Someone who has never heard of MeshCore installs the app from the Play Store,
pairs it with a radio in under two minutes, and starts messaging — with no
internet, no account, and no understanding of how a mesh works. Everything
complicated (protocol, encryption, routing, keys, radio settings) is either
invisible or safely tucked behind an "Advanced" door.

## Target users

1. **The newcomer (primary).** Bought or was given a radio. Wants WhatsApp-like
   messaging that works off-grid. Does not know what an advert, a flood route,
   or a channel key is — and never has to.
2. **The group organizer.** Sets up channels for a hiking club, farm co-op,
   event, or emergency group. Needs QR/links to share channels, per-channel
   notifications, and read/unread clarity.
3. **The mesh enthusiast (advanced).** Traces paths, tunes path hash size,
   imports/exports identities, runs a connected node as an app-driven
   repeater. Wants these features to exist *without* cluttering the main screens.

## Product principles (ordered — when they conflict, higher wins)

1. **Off-grid by default.** The app is fully usable with no radio connected
   (read history, compose, manage identities/channels) and requires zero
   internet. The only internet use is optional (Play Store install, optional
   cloud backup).
2. **One-tap to first message.** Every step between "install" and "sent
   message" must be unavoidable-but-brief: connect radio → name yourself →
   message. Nothing else is mandatory.
3. **Hide the mechanics.** No jargon in the primary UI. "Advert" becomes
   "Share my contact"; "path hash size" lives in Advanced; the concept of
   regions is reduced to picking your area from a short list during setup.
4. **The radio is dumb on purpose.** All protocol in the app. Firmware changes
   are rare; app changes ship via the Play Store.
5. **No artificial limits.** Unlimited contacts, channels, and message history
   because everything is stored in the app database, not on the radio.
6. **Modern, calm Android UI.** Material 3, large touch targets (≥48 dp),
   generous type, minimal chrome. Follow platform conventions so the app feels
   native, and UX best practices so features are discoverable without docs.
7. **Layered, testable code.** Presentation / domain / protocol / transport are
   strictly separated so each layer is unit-testable and swappable.
8. **Minimal, explainable scope.** Every feature must map to a stated user
   need. "No companion telemetry" and "no official internet map" are explicit
   non-goals.

## Explicit non-goals

- Companion telemetry / sensor readouts from companion nodes.
- The official MeshCore internet map.
- iOS in v1 (architecture stays portable, but Android-only at launch).
- Any server, account, or phone number. The mesh is the network.
- Firmware implementing any mesh protocol — the radio never replays,
  retransmits, or transmits anything on its own initiative.

## Success criteria

- A newcomer sends their first message in < 5 minutes from install with only
  the app's own onboarding.
- App is fully functional offline: history, compose, channel/contact
  management all work with no radio and no internet.
- Protocol layer has ≥ 90 % unit-test coverage; firmware has host-side unit
  tests plus an automated hardware smoke test.
- Firmware can go months between updates; app updates weekly if needed.
- A user who loses their phone can restore identity + full message history
  from backup (advanced but supported).
