# 11 — Gap Analysis: MeshCore Open (`meshcore-open/`)

Source studied: `~/dev/meshcore/meshcore-open` (Flutter client by zjs81, with
documentation in `documentation/` and `docs/BLE_PROTOCOL.md`). Purpose: mine it
for problems we *will* hit, make our decisions now — **not** to copy its UX
(the user's stated design goal rules that out; several of its choices are
wrong for MeshPigeon and are listed as rejections).

## 1. Fundamental architecture differences (drive everything below)

| Aspect | MeshCore Open | MeshPigeon (our plan) |
|---|---|---|
| Identity & keys | Live **on the radio firmware** | Live **in the app** (keystore); radio never holds keys |
| Contacts/channels | Stored on device firmware (limited: 40 channels, fixed contact slots) | Unlimited, in app DB |
| ACK handling | Firmware has an 8-entry ACK table → app must serialize sends per contact | No ACK table (dumb radio) → app-side ACK tracking, queueing is a design choice not a firmware limit |
| Connection model | Scanner home screen; manual connect each time | Radio preference order + auto-connect (05 §2) |
| Unread state | Scoped to **connected device's** pubkey (switching radios resets) | Scoped per identity (correct behavior) |
| Identity switching | Not supported (identity = device) | First-class multi-identity (06 §3) |
| Protocol layer | Split app/firmware; companion protocol | 100 % in app; dumb-radio command spec (04 §4) |

Consequence to exploit: MeshCore Open's most annoying constraints (one
in-flight message per contact, 40-channel cap, per-device unread) simply don't
apply to us. Decisions §2.4 and §3 below bank that advantage.

## 2. Decisions — ADOPT (adapted, not copied)

### 2.1 Retry/ACK engineering (from their Message Retry Service + timeout docs)
MeshCore Open's matured answers to "when do I retransmit, and when do I give
up?" are the best source in the repo. Adopt into `:core-protocol` (03 §4):
- **Physics-based timeout estimate:** `500 + (airtime × 6 + 250) × (hops + 1)` ms
  direct, `500 + 16 × airtime` ms flood, computed from current SF/BW/CR; cap at
  45 s. (They layer ML prediction on top — we reject that; see §4.)
- **Exponential backoff** `1s × 2^n`, configurable max retries (default 5).
- **30-second grace window** after "failed": a late ACK still flips the message
  to confirmed. Cheap to build, prevents the classic "showed failed but
  arrived" complaint.
- **Duplicate-ACK hash history** so a re-broadcast ACK doesn't double-confirm.
- **Manual "Retry" on failed messages** (long-press), preserving original text.
- Updates: 03 §4 and 07 §4.

### 2.2 Per-conversation send queue (soft)
Not a firmware limit for us, but a product decision: **one in-flight message
per conversation**, rest FIFO-queued (prevents air-time hogging and keeps
delivery states honest). Multiple conversations can send in parallel.
→ 03 §4.

### 2.3 Path engineering, simplified (from Path History Service + routing-paths.md)
They keep a scored path cache (reliability 45 % / weight 20 % / latency 25 % /
freshness 10 %), route rotation on retries, and manual per-contact override
(auto / direct / flood). Adopt a **reduced version**:
- v1 core (already planned): path cache in DB; direct-if-known, else flood.
- v1 addition: on send failure, **clear the failed path** and retry via flood
  (their "clear path on max retry", reduced to automatic).
- Advanced (08): manual routing override per contact (Auto/Direct/Flood) and
  a path list ("N hops, last used 5 m ago, ✓ 12/13"). No rotation, no scoring
  weights — only if field data shows it's needed.
- Their on-air gotcha is worth engineering notes: `path_len` encodes
  hop-count × hash-width; **never** treat the byte length as hop count.
  Add as a code comment mandate + test in golden vectors (09).

### 2.4 Channel types (from channels.md)
Their taxonomy is useful; ours maps to it: Public / hashtag ("shared") /
private. **Adopt**: hashtag-style name-derived keys and paste-tolerant key
input ("strip spaces/dashes when pasting 32-hex keys"). Already in 07 §6;
add the paste-tolerance detail.

### 2.5 Notification engineering (from notifications.md)
Adopt: 3-second minimum interval + batch summary notification ("3 messages,
2 new contacts") to prevent advert/message storms; deep-link tap →
**navigate directly to the conversation** (they launch to root — ours will do
better). Per-conversation modes already planned; keep "no per-contact muting"
question answered: we **do** per-DM muting (per-conversation setting covers
DMs and channels uniformly — simpler model than theirs). → 07 §8.

### 2.6 Contact intake (from contacts.md / discovery screen)
Adopt: passive-heard contacts flow into **Pending contacts** (already planned);
add — bulk "clear all" with confirmation; **import from clipboard** of a
`meshcore://`-style URI; **zero-hop advert** ("Say hi to nearby radios") as a
one-tap action next to "Share my contact". → 07 §5/§7, 03 §3.

### 2.7 Region presets (from settings.md)
Their preset list (per-country incl. narrow variants) validates the regions
approach; adopt: bundle a comparable preset table in-app; custom stays behind
Advanced; presets also importable via QR/link. → 03 §8, 07 §12.

### 2.8 Radio battery as *device status* (from Device Info card)
Their battery monitoring is good UX and **is not mesh telemetry** — it's
status of the thing you're plugged into. Decision: firmware `GET_INFO` gains
an optional battery mV/percent field (one ADC read — trivially dumb); the
connection chip shows "Pocket Node · 84 %" and warns ≤ 15 %. This does **not**
reopen the no-companion-telemetry non-goal (that's about on-air sensor
payloads). → 04 §4, 07 §1.

### 2.9 Misc UX adoptions
- **Mark as unread** context action (their "Mark as Unread" long-press). → 07 §4.
- **Delete message locally** context action. → 07 §4.
- **Jump-to-bottom button, lazy message loading, bubble width cap (~72 %)**. → 07 §4.
- **Message tracing mode**: their opt-in "extra metadata in bubble" validates
  our sub-info line; we additionally show **RTT on delivered DMs** in Message
  details. → 07 §4.
- **Linkify + URI import**: `meshcore://`-style clipboard import wired into
  Start-chat sheet and Contacts overflow. → 07 §11.
- **Localization infrastructure from day one**: strings in ARB resources with
  English-only content at launch; languages are cheap later. → 06 §7.

## 3. Decisions — DEFER (parked with an owner doc reference)

| Feature | Theirs | Our decision |
|---|---|---|
| **Images over mesh** | Chunked image transfer w/ custom RANS entropy codec, blob stores | Real gap in MeshCore UX, but very expensive on-air. Deferred to post-M5 stretch; our MULTIPART payload type (03 §1) is reserved for it. Compose plus-menu keeps "Photo (stretch)". |
| **SMAZ text compression** | Opt-in per contact/channel; garbles for non-MeshCore-Open clients (`s:` prefix) | Reject the interop-garbling default; revisit as Advanced, off by default, never for channels strangers use. |
| **"Guessed locations"** (infer contact position from repeater-path anchors) | Clever repeater-anchor triangulation w/ confidence levels | Genuinely good idea for a map where most nodes lack GPS. Park as Advanced map option post-M5 (08). |
| **Communities** (HMAC-derived channel namespace from one secret) | A group secret derives a whole set of channels, shareable by one QR | Attractive, but v1 keeps single-channel sharing (07 §6). Park: a future "Group" = one secret → multiple channels, one QR. |
| **GPX export of contacts/repeaters** | Settings → Export | Park with 08 backups/exports; trivial addition. |
| **Managing MeshCore repeaters (CLI hub)** | Full repeater admin over mesh | Out of scope — our firmware has no repeater protocol, and MeshCore repeater admin is a separate protocol surface. Revisit only if the app later speaks MeshCore repeater CLI too. |
| **iOS / desktop / web** | 6 platforms | Android-only v1 (02); layering keeps the option open. |

## 4. Decisions — REJECT (documented so we don't relitigate)

| Their feature | Why we reject |
|---|---|
| **ML timeout prediction** (sliding-window regression, 1.5× margin) | Physics formula + backoff (2.1) covers it; ML adds nondeterminism that fights our testability principle. |
| **Giphy GIF picker** (`g:<id>` wire format, CDN rendering) | Requires internet + a third-party service — violates off-grid-by-default. Our link-preview-on-tap (07) is the compromise. |
| **On-device LLM translation** | Heavyweight, out of product scope; parked forever unless users ask. |
| **Text-prefix in-band message types** (`r:`, `m:`, `g:` in cleartext channel text) | Reactions/pins render as garbage to non-app clients. Our GRP_DATA sub-messages (03 §6) are invisible to other clients — cleaner; we accept that other clients never see reactions at all. Matches the user's "look proper for non-app users" requirement better than theirs does. |
| **Scanner as home screen** | Forces a connect-first mental model. Our offline-first home is Chats (00 principle 1/2). |
| **Per-device-scoped unread/state** | Correctness bug in a multi-identity world; ours is identity-scoped (06 §2). |
| **Contact favorites/stars, client-side contact groups** | Chats pinning + search covers the need; extra organizational UI violates minimalism. Revisit if users ask. |
| **Path scoring weights, auto route rotation (default-on)** | Opaque knobs that fail "hide the mechanics". Keep Auto/Direct/Flood override in Advanced only. |

## 5. Plain gaps in *our* plan this analysis closed

1. Timeout physics + grace window + dup-ACK handling (03 §4).
2. Per-conversation FIFO send queue (03 §4).
3. Auto path-clear on exhaustion; manual retry; Advanced routing override (03/07/08).
4. Notification batching + deep links (07 §8).
5. Clipboard contact URI + zero-hop "say hi nearby" (07 §5).
6. Radio battery as connection status (04 §4, 07 §1).
7. Start-on-boot for the connection service — their background service does
   *not* auto-start after reboot; MeshPigeon will offer "Reconnect after phone
   restart" in Settings (06 §6).
8. i18n scaffolding at launch (06 §7).
9. Region preset table breadth (03 §8).
10. Engineering note: never equate path byte-length with hop count (09 golden tests).

## 6. What we deliberately do better (unchanged, for confidence)

Unlimited contacts/channels/history (app-side storage); multi-identity without
touching radios; radio preference order + silent fallback; offline-first home;
keys never on the radio; dumb firmware = tiny attack surface and rare updates;
per-DM and per-channel notification control; write proper replies/reactions
that don't garble non-app clients.
