# 10 — Roadmap, Repo Setup & Guiding Principles

## 1. Repository scaffolding

Two public repos (under a `meshpigeon` GitHub org or `jhuebert` namespace):

```
meshpigeon-app/         Kotlin, Gradle multi-module, AGP/Kotlin pinned, CI: GitHub Actions
  GUIDING-PRINCIPLES.md   ← adapted from 00-vision (app-flavored)
  docs/  (user docs live here too — see §4)
meshpigeon-firmware/    PlatformIO, boards/<board>/, host tests, CI: build-all + host tests
  GUIDING-PRINCIPLES.md   ← firmware-flavored
```

Both: MIT/Apache-2.0 choice recorded in the first commit; CONTRIBUTING.md per
repo; this plan set imported into each repo's `docs/plans/` as the founding
documents.

## 2. Milestones

| M | Name | Contents | Exit criteria |
|---|---|---|---|
| M0 | Foundations | Both repos scaffolded, CI green, `RadioAdapter` SPI + framing codec + simulator skeleton | A `PING` round-trips over BLE to a real board |
| M1 | Talk | Firmware packet store + settings persistence; app: onboarding, connect, public channel, send/receive TXT_MSG, uptime mapping, Chats UI v1 | Two phones message over one radio, plus history catch-up on reconnect |
| M2 | People | Identities (multi), adverts + contacts, DMs with crypto, ACK states, notifications, blocking, search | DM between two MeshPigeon phones via flood; strangers' DMs create requests |
| M3 | Channels | Channel create/rename/delete/pin, QR/links, reactions/replies per 07, map v1, mark-all-read | Channel round-trip incl. QR join on a second device |
| M4 | Dumb-radio polish | App-driven repeater (app-side dedup + rebroadcast), multi-client sharing/epoch (05 §3), radio preference order, character budget UI, message details | Bench: 3 phones × 1 radio incl. settings-epoch and repeater flows |
| M5 | Advanced & durability | Trace path, path-size setting, import/export, optional Drive backup, location share, firmware update in-app, perf/battery pass | All Advanced features demo-able; 72 h soak passes |
| M6 | Release | Docs, Play internal → closed testing, interop field test vs MeshCore | Success criteria in 00 met on real devices |

Re-visit scope after M2; M5 items are individually droppable without harming
the core product. Post-M5 stretch pool (from 11 §3): images over mesh,
guessed map locations, communities, GPX export.

## 3. Per-repo GUIDING-PRINCIPLES.md (both repos carry this verbatim header)

> **MeshPigeon exists so a stranger to mesh radio can install, connect, and
> message in minutes — offline, forever.**
>
> 1. Protocol in the app, never the firmware. Firmware stays dumb and durable.
> 2. Hide mechanics; expose outcomes. Users see "heard ✓", not "flood route ACK".
> 3. Offline-first: the app must always be useful with no radio and no internet.
> 4. No artificial limits: history, contacts, channels live in the app DB.
> 5. Layers stay separated (UI / domain / protocol / transport) and tested;
>      `:core-protocol` never imports Android.
> 6. Minimal diffs; match existing style; every change maps to a stated need.
> 7. Accessibility is a requirement: ≥48 dp targets, screen-reader labels,
>      large type support.
> 8. When features conflict, order: newcomer experience → reliability →
>      enthusiast features.

Firmware addendum:

> The radio stores packets it cannot read and persists its settings; it never
> transmits on its own initiative. If a proposed change gives the firmware
> opinions about repeating, message content, keys, or identity — stop; it
> belongs in the app.

## 4. Documentation plan (repo `docs/`, user-facing)

- `getting-started.md` — plain-language install → connect → first message
  (with screenshots); zero assumed knowledge.
- `radios.md` — supported boards, flashing for the first time, updating.
- `channels-and-contacts.md` — channels, sharing QR/links, blocking.
- `troubleshooting.md` — "my message shows ✓ but no reply", radio not found,
  region questions.
- `advanced.md` — tracing, path size, backups, custom radio settings.
- `radio-protocol.md` (firmware repo) — the dumb-radio command spec (04 §4)
  as a public, versioned contract.

## 5. Engineering practices (both repos)

- Trunk-based, short-lived branches, conventional commits, PR templates
  mapping changes to requirements.
- Static analysis gates: detekt/ktlint (app), clang-format + cppcheck (fw).
- Semantic versioning; app keeps a `PROTOCOL_VERSION` constant aligned with
  the firmware command spec (documented in `radio-protocol.md`).
- Every merged feature adds or extends a test named for the requirement
  (traceability matrix in `docs/testing-map.md`).

## 6. Risks & mitigations

| Risk | Mitigation |
|---|---|
| MeshCore on-air incompatibility | Golden vectors + bench interop from M1 (09 §4) |
| BLE multi-client quirks across phones | Matrix testing on cheap/old devices in M4 |
| Uptime drift / clock mapping errors | Anchor protocol + adversarial tests (09 §1) |
| Scope creep in advanced features | M5 items droppable; advanced isolated in its own module |
| Firmware-brain creep | Principle 1 is a review gate, not a suggestion |
