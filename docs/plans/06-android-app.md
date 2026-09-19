# 06 — Android App Architecture & Data Model

Repo: `meshhop-app`. Native Kotlin + Compose, layered per 02. This doc covers
the app skeleton, persistence, and identity handling.

## 1. Module → responsibility recap

- `:core-protocol` — packet codec, crypto, routing FSM, ACK tracking (pure Kotlin).
- `:core-domain` — entities, use cases, repository interfaces (pure Kotlin).
- `:core-data` — Room, datastore, backup/export implementations.
- `:core-transport` — RadioAdapter SPI + BLE/USB/TCP.
- `:feature-*` — Compose UI + view-models per feature.
- `:app` — DI (Hilt), navigation, foreground service wiring.

## 2. Persistence (Room)

Everything lives on-device; there is no size limit mirroring radio constraints.

```sql
identities(id PK, name, pubkey BLOB, privkey_enc BLOB, flags, created_at, is_active,
           advert_policy, last_advert_at)
conversations(id PK, kind TEXT)            -- kind: dm | group | public_ | trace_log
conversation_members(conv_id, contact_id, joined_at)
contacts(id PK, pubkey UNIQUE, name, first_seen_at, source, blocked_at NULL,
         last_seen_at, flags, note)
channels(id PK, name, key_enc BLOB, kind TEXT,   -- named | shared | public_
         created_at, pinned, muted, notify_mode)
messages(id PK, conv_id FK, sender_contact_id NULL, body TEXT, kind TEXT,
         sent_at INTEGER,          -- wall clock
         recv_uptime_anchor INTEGER,      -- provenance
         out bool, msg_state TEXT,        -- composing|sent|heard|confirmed|failed
         snr REAL, rssi INTEGER, hops INTEGER, region TEXT, path_info BLOB,
         reply_to_id NULL, reactions BLOB, edited_at NULL)
outbox(id PK, conv_id, packet BLOB, ack_key BLOB, attempts, next_retry_at,
       state TEXT)
radio_targets(id PK, persistent_id UNIQUE, name, link_kind, link_addr,
              pref_order, preferred, desired_radio_settings JSON, last_connected_at)
packet_tags(tag BLOB PK, seen_at)          -- app-side dedup cache
settings(key PK, value)
```

- Private keys: `privkey_enc` sealed with Android Keystore (StrongBox where
  available). Backup/export re-wraps with a user passphrase (08 §3).
- Full-text search: Room FTS4/5 virtual tables over `messages.body`,
  `contacts.name`, `channels.name` (07 §Search).
- Migrations forward-only; schema exported for review.

## 3. Identity model

- **Multiple identities** are first-class (requirement: change radios without
  changing identity, and vice-versa). An identity switch is a single tap in the
  drawer; conversations/contacts/channels are **per identity** (namespaced by
  `identity_id` added to every table above — shown simplified).
- Active identity defines: advertised name, keys, channel set, and the radio
  preference list is *shared across identities* (so switching radios never
  forces identity churn — the core reason for the split).
- **Import/export** (Advanced): encrypted `.meshhop-identity` file + QR
  (08 §3).
- **Adverts, dumbed down:** a single "Share my contact" action on the profile
  and in the plus-menu; automatic re-advert only when the user enables
  "let people find me nearby" (periodic flood advert, default off). Contact
  cards as attachments make sharing explicit (07 §Compose).

## 4. Domain services

| Use case | Notes |
|---|---|
| `ConnectToRadio` | runs preference-order selection (05 §2), owns foreground service |
| `SyncRadioHistory` | `FETCH_PACKETS(since_seq)` on connect → dedup → decode → persist; backfills conversation unread counts |
| `SendMessage` | builds packet (protocol), inserts outbox, schedules retries, maps ACK → state transitions |
| `ReceivePipeline` | frame → dedup → decode → routing (DM/group/public/advert/ACK/trace) → storage + notification decisions |
| `ContactDirectory` | merge adverts into contacts, blocked list, pending contacts |
| `ChannelManager` | create/rename/delete/pin, join via QR/link (07) |
| `BackupService` | optional; cloud/Drive (08 §4) |

The `ReceivePipeline` and `SendMessage` are pure-Kotlin orchestrators
injected with repository interfaces — the heart of the test strategy (09).

## 5. Offline-first behavior

- No radio connected: compose (goes to outbox), read full history, search,
  manage channels/contacts/identities, view map of known positions — all work.
  A banner notes the radio is offline; outbox flushes on next connect.
- No internet ever required; Play install is the only online step in normal
  life. Crash reporting/analytics: **none**.

## 6. Background & lifecycle

- `RadioConnectionService` (foreground): holds adapters, syncs, posts
  notifications per per-channel/per-DM notification prefs (07 §Notifications).
- Battery: BLE callbacks drive work; no polling loops; JobScheduler only for
  optional backup.
- Process death: outbox + retry schedule are DB-backed, so messages survive
  restarts.
- **Reconnect after phone restart:** optional Settings toggle registers a
  BOOT_COMPLETED receiver that relaunches the foreground service (competitor
  apps skip this — a connected radio should come back after a nightly phone
  reboot).

## 7. Design system & accessibility

- Material 3, dynamic color, one accent; large-text friendly (sp everywhere);
  ≥ 48 dp targets; TalkBack labels on all icon buttons; supports font scaling
  to 200 %; gesture + button nav both first-class (07 has the screen specs).
- **All user-facing strings live in ARB resources from day one** (English at
  launch) so additional languages are a translation task, not a refactor.
