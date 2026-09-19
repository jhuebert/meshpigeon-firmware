# 02 — System Architecture

## Big picture

```
┌────────────────────────────────────────────────────────────┐
│                       Android phone                        │
│  ┌──────────────────────────────────────────────────────┐  │
│  │                 MeshHop App (Kotlin)                │  │
│  │                                                      │  │
│  │  ┌────────────┐  ┌──────────┐  ┌──────────────────┐  │  │
│  │  │  UI layer  │  │  Domain  │  │  Protocol layer  │  │  │
│  │  │ (Compose)  │←→│ (logic,  │←→│  (packet codec,  │  │  │
│  │  └─────┬──────┘  │ storage) │  │   crypto, routing│  │  │
│  │        │         └────┬─────┘  │   state machines)│  │  │
│  │        │              │        └────────┬─────────┘  │  │
│  │        │        ┌─────▼─────────────────▼──────┐     │  │
│  │        │        │      Transport layer         │     │  │
│  │        │        │  (RadioAdapter interface:    │     │  │
│  │        │        │   BLE, USB-CDC, Wi-Fi/TCP)   │     │  │
│  │        │        └──────────────┬───────────────┘     │  │
│  │  ┌─────▼───────────────────────▼─────────────────┐   │  │
│  │  │  Room DB (messages, contacts, channels,       │   │  │
│  │  │  identities, radio profiles) + files          │   │  │
│  │  └───────────────────────────────────────────────┘   │  │
│  └──────────────────────────────────────────────────────┘  │
│            │ BLE / USB / Wi-Fi (raw byte transport)        │
└────────────┼───────────────────────────────────────────────┘
             ▼
┌────────────────────────────────────────────────────────────┐
│                MeshHop Radio Firmware                     │
│  LoRa radio ─ packet store (max packets) ─ raw frame transport│
│  No protocol, no keys. Persists radio settings.            │
└────────────────────────────────────────────────────────────┘
             ▼
        (on-air, MeshCore-compatible flood/direct routing)
```

## Two repositories

| Repo | Contents | Release cadence |
|---|---|---|
| `meshhop-app` | Android app; also contains a shared `protocol` spec doc and golden test vectors | Play Store; frequent |
| `meshhop-firmware` | Firmware for supported boards; host-side simulator + tests | Rare; OTA/USB/BLE update when needed |

Both repos get a copy of `GUIDING-PRINCIPLES.md` (see 10-roadmap) derived from
[00-vision](00-vision-and-principles.md).

## Tech stack decision: native Android (Kotlin) — chosen

Decision: **Kotlin + Jetpack Compose, single module per layer** (multi-module
Gradle). Rationale against the user's open question (native vs Flutter vs RN):

| Requirement | Native Kotlin | Flutter | React Native |
|---|---|---|---|
| Multi-BLE concurrent connections | First-class (Android BLE APIs) | Wrap platform APIs anyway | Same |
| USB-CDC serial (OTG) | Direct | Plugin | Plugin |
| Flash firmware from app (ESP32/nRF DFU) | Mature libs (nRF DFU; esptool port feasible) | Wrapper | Wrapper |
| Foreground service, Doze handling, exact alarms | Native | Escalation | Escalation |
| Material 3 / platform feel | Native by definition | Close | Loser |
| Long-term maintainability by one dev | Single language/platform, best tooling (Android Studio) | Fine | Fine |

Flutter was the runner-up (portability to iOS later); the decisive factors are
BLE/USB/DFU plumbing depth and "proper modern Android app" as a hard
requirement. The layered design keeps domain/protocol/transport pure-Kotlin
(Java/Kotlin-lib friendly) so a future Flutter or iOS client could reuse the
*specification*, even if code is rewritten.

## App layering (enforced by module boundaries)

```
:app                 → composition, DI wiring, navigation
:ui-core             → Compose theme, design system, reusable components
:feature-messaging   → chat list, conversation, compose
:feature-contacts    → contacts, channels, map, settings
:feature-onboarding  → first-run, radio connect, region pick
:core-domain         → entities, use cases, repository interfaces (pure Kotlin)
:core-protocol       → MeshCore-compatible codec, crypto, routing FSM (pure Kotlin, no Android)
:core-transport      → RadioAdapter SPI + BLE/USB/TCP implementations
:core-data           → Room DB, repositories, backup/export
```

Rules:
- `:core-protocol` and `:core-domain` must compile on the JVM with **no Android
  SDK** — this is what makes them unit-testable at 90 %+ coverage.
- `:core-transport` defines `RadioAdapter` (see 05) — nothing above it knows
  whether the radio is BLE, USB, or Wi-Fi.
- The protocol layer never touches the DB; the domain layer translates protocol
  events into storage mutations and UI state.

## Threading & process model

- **Foreground service** while a radio is connected: keeps BLE alive and
  processes packets when the app is backgrounded; shows a persistent
  notification with connection state (required by Android; kept calm and
  informative).
- Packets are decoded on a dedicated dispatcher; DB writes are serialized via
  Room; UI observes `Flow`s.
- All persistence is local (Room + files). No network code exists outside the
  optional cloud-backup module (08).

## Firmware architecture (see 04 for detail)

- Single binary, board ports for ESP32-S3 (XIAO WIO class) and nRF52 first.
- Modules: `radio` (LoRa), `packetstore` (ring buffer, flash or RAM),
  `transport` (BLE UART / USB CDC / Wi-Fi TCP, line/SLIP framing),
  `settings` (NVS/flash persistence), `clock` (uptime counter).
- The firmware **never parses payloads** beyond a 4-byte dedup tag.
