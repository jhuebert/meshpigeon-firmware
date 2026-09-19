# 08 — Advanced Features

Everything here lives behind Settings → **Advanced** (or clearly-marked
disclosure controls) so the main screens stay calm. Grouped in an Advanced
screen with short, plain-language descriptions.

## 1. Path tracing

- Uses protocol TRACE payload (03 §1) to walk a path and collect per-hop SNR.
- Entry points: Message details → "Trace path to this user"; Contacts →
  contact → "Trace path". Results render as a hop diagram (hop, node hash,
  SNR) and are stored in a `trace_log` conversation kind for reference.
- Never surfaces in main navigation. Progress + cancel affordances; timeout
  explains "no reply within range/3 hops".

## 2. Path hash size

- Default **3-byte** path hashes (03 §2). Advanced → "Routing" exposes:
  - Show effective path hash size per conversation.
  - Downgrade to 2-byte or 1-byte (legacy interop) with a warning about
    collision/false-positive risk at density.
  - Per-destination override for problem links.
- All packet-encoding consequences are unit-tested in `:core-protocol`.

## 3. Identity import/export & backup

- **Export identity:** encrypted file `.meshhop-identity` (AES-GCM, key from
  user passphrase via Argon2id/PBKDF2) containing keys + name + flags.
  Shareable as file or QR (QR only for passphrase ≤ short, warns).
- **Export full backup:** identity + contacts + channels + message history
  (`.meshhop-backup`, schema-versioned).
- **Import** restores on a new device; prompts about merging vs replacing.
- **Cloud backup (optional, off by default):** Google Drive App Folder via
  Drive API — app-scoped, user-encrypted backup blobs on a schedule or
  manual. Requires internet by definition, which is why it's opt-in and the
  local export path is equally good. No other cloud providers in v1.

## 4. Companion location (advert only)

- Advanced → "Share my location": sets a static position included in adverts
  (or on demand as an advert refresh), never streamed.
- UI: pick on map or paste coordinates; one-line explainer "Others will see
  you on the map." Off by default. No telemetry of any other kind (hard
  non-goal, 00).

## 5. Other advanced settings inventory

| Setting | Default | Notes |
|---|---|---|
| Routing override per contact (Auto / Direct / Flood) + path list | Auto | Path list shows hops, last-used, success count; on send exhaustion the learned path is cleared automatically (03 §4) |
| Radio settings editor (freq/BW/SF/CR/power) | hidden behind region picker | 05 §4 |
| Auto re-advert interval | off | "let people find me nearby" |
| Show signal details on messages | off | sub-info line (07 §4) |
| Packet store purge | manual | Advanced → connected radio |
| Firmware update | manual | 04 §5 |
| Protocol log viewer | off | decoded packet inspector for debugging |
| Trace path | n/a | §1 |

## 7. Deferred features (studied in meshcore-open — see 11 §3)

Parked, with owners in the roadmap if field demand appears:

- **Images over mesh** — chunked transfer on the MULTIPART payload type;
  expensive on-air; stretch post-M5.
- **Guessed locations** — infer contact positions from repeater-path anchors
  when GPS is absent; Advanced map option.
- **Communities** — one shared secret deriving a set of channels, shared by a
  single QR.
- **GPX export** of contacts/repeaters alongside backups.

## 8. Guardrails

- Every advanced screen opens with one sentence of consequence ("Changing
  path size changes how far your message can travel").
- Advanced screens are excluded from onboarding paths entirely; discoverable
  but never required.
