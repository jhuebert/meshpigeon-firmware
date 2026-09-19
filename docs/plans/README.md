# MeshHop — Project Plan Index

**MeshHop** is a mesh-radio messaging system for people who have never heard of
MeshCore: install the app, connect a radio, start messaging. It consists of
exactly two deliverables:

1. **MeshHop App** — a native Android app. All protocol, identity, storage, and
   UX intelligence lives here. (Repo: `meshhop-app`)
2. **MeshHop Radio Firmware** — the bare-minimum firmware for a LoRa radio
   board: fetch packets, send packets, set radio, survive reboots, remember
   history. No protocol, no keys. (Repo: `meshhop-firmware`)

The plan documents live in this directory. Read them in order for a first pass;
each is self-contained for its topic.

| File | Topic |
|---|---|
| [00-vision-and-principles.md](00-vision-and-principles.md) | Product vision, target user, guiding principles, success criteria |
| [01-naming.md](01-naming.md) | Name candidates, availability, recommendation (**MeshHop**) |
| [02-system-architecture.md](02-system-architecture.md) | Two-repo split, layered architecture, tech stack decision |
| [03-mesh-protocol.md](03-mesh-protocol.md) | On-air protocol (MeshCore-compatible), crypto, packet handling in the app |
| [04-firmware.md](04-firmware.md) | Firmware spec: packet memory, uptime timestamps, settings persistence, repeat mode |
| [05-transport-and-connections.md](05-transport-and-connections.md) | Adapter interface (BLE / USB / Wi-Fi), multi-radio preference, concurrent connections |
| [06-android-app.md](06-android-app.md) | App module structure, data model, identities, offline-first storage |
| [07-ux-and-features.md](07-ux-and-features.md) | Screens, flows, message features, notifications, blocking, map, QR/links |
| [08-advanced-features.md](08-advanced-features.md) | Path tracing, path hash size, identity import/export, backup, companion location |
| [09-testing.md](09-testing.md) | Test strategy for app and firmware, protocol conformance, hardware rig |
| [10-roadmap-and-repo-setup.md](10-roadmap-and-repo-setup.md) | Milestones, repo scaffolding, CI, release model, per-repo guiding principles |
| [11-meshcore-open-gap-analysis.md](11-meshcore-open-gap-analysis.md) | Study of the meshcore-open Flutter client: adopted / deferred / rejected decisions |

## The one-paragraph summary

A dumb, durable radio and a smart app. The firmware speaks no mesh protocol: it
receives packets into the largest memory it can hold, stamps each with uptime,
and persists its radio settings. The app speaks the whole
MeshCore-compatible protocol — adverts, encryption, flood/direct routing,
ACKs, channels, contacts, and the optional repeater function — so protocol
fixes ship via the Play Store, and the firmware rarely changes. Users see none
of this: they see a clean messaging app with a chat list, a map, and a big
"Start chat" button.
