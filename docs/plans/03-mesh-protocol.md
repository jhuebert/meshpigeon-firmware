# 03 — Mesh Protocol (implemented entirely in the app)

The app speaks a **MeshCore-compatible v1 on-air protocol**, so MeshHop users
can interoperate with the existing MeshCore ecosystem (repeaters, companions).
Reference: `MeshCore/docs/packet_format.md`, `payloads.md`, `companion_protocol.md`
in the local MeshCore clone. We use it as the *wire spec*; none of MeshCore's
companion-app UX is copied (per design goals).

Firmware is protocol-free; everything below lives in `:core-protocol`.

## 1. Packet framing (v1 format)

```
[header:1][transport_codes:0/4][path_length:1][path:0..64][payload:0..184]
```

- **Header** `0bVVPPPPRR`: version (2 bits), payload type (4 bits), route type
  (2 bits).
- **Route types:** flood (0x01), flood+transport (0x00), direct (0x02),
  direct+transport (0x03). MeshHop v1 emits plain flood and direct;
  transport codes are accepted and preserved on rebroadcast but not generated.
- **Path length byte:** bits 0–5 hop count, bits 6–7 hash size code
  (1/2/3-byte path hashes; default **3-byte** per product decision, downgrade
  allowed in Advanced — see 08).
- **Payload types used by the app:**

| Type | Name | App usage |
|---|---|---|
| 0x02 | TXT_MSG | Plain (cleartext) public broadcast |
| 0x03 | ACK | Message acknowledgments |
| 0x04 | ADVERT | Identity advertisement (name, pubkey, flags) |
| 0x05 | GRP_TXT | Encrypted channel text |
| 0x06 | GRP_DATA | Encrypted channel data (reactions, typing, timestamps — see §6) |
| 0x07 | ANON_REQ | Unsigned request to a node (e.g. fetch stored packets, status) |
| 0x00/0x01 | REQ / RESPONSE | Signed request/response to a known node |
| 0x08 | PATH | Returned path (reply-path learning) |
| 0x09 | TRACE | Path trace (Advanced feature, see 08) |

Payload ceiling: **184 bytes**; total airtime packet ≤ 184 + header/path.
Message content longer than one payload is *not* fragmented on-air in v1 — the
compose box enforces the per-protocol character budget and shows remaining
count (see 07).

## 2. Identity & keys (app-held only)

- Identity = Curve25519 keypair + human name (≤ 67 bytes on-air) + flags.
  Keys live **only** in the app keystore / encrypted DB. Never sent to the radio.
- Private-channel (DM) crypto: shared secret = X25519(myPriv, theirPub);
  symmetric stream cipher + MAC per MeshCore DM scheme. Outgoing DMs include
  dest hash + MAC; receivers try their own keys to open.
- Channel crypto: channel name → shared key (KDF on name, MeshCore-compatible
  channel scheme); messages are GRP_TXT/GRP_DATA keyed by 1-byte channel hash.
- "Public" is the special well-known channel present on first launch.
- No keys are ever written to the radio. A stolen radio leaks nothing.

## 3. Adverts ("Share my contact")

- Flood-routed ADVERT carrying pubkey, name, flags (companion/repeater bits),
  optional timestamp; optionally sent with a path (RPN-gated in Advanced).
- App UI treats adverts as a plain-people concept: **"Share my contact"**
  (pushes your card out) and a contact card **attachment** you can drop into
  any chat (see 07). Auto-advert cadence configurable per identity
  (default: manual + on-demand via the plus menu), so new users never see a
  background advert storm.
- Incoming adverts from unknown pubkeys create **pending contacts** the user
  can accept or ignore (blocking = drop pubkey permanently, see 07).

## 4. Routing state machine (app-side)

- **Send (flood):** build packet, record `expected ACK = CRC(timestamp+salt)`
  for direct TXT/GRP_TXT; store in outbox with retry schedule
  (e.g. t+2s, t+5s, t+15s) until ACK or expiry → UI shows
  ⏳ sending → ✓ heard → ✓✓ confirmed (mesh relay heard it — see §7).
- **Send (direct):** if a learned path exists for the destination (from
  adverts/ACK paths/PATH replies), send direct-routed with that path; on
  timeout fall back to flood. Path cache lives in the app DB.
- **Receive:** dedup by packet tag before decode; route to handler by payload
  type; everything decoded is persisted with **reception metadata** (see §7).
- **Repeating (the in-app repeater):** repeating is a **pure app feature**. The
  app dedups incoming packets by tag and re-sends eligible ones via
  `SEND_PACKET` (transport codes preserved). The firmware has no repeat logic
  at all (04 §1.3) — it only transmits what the app tells it. Consequence:
  repeating works only while the app is connected, which the product accepts
  (standalone always-on infrastructure remains the job of real repeaters).
  Optional app-side filter rules (prefix/channel policy) become possible later
  precisely because the decision point is in the app.

## 5. Radio-node requests (the "dumb radio" API)

Because the firmware holds no protocol, the app drives it via simple
request/response over the transport (BLE/USB/TCP), *not* mesh packets:

| Command | Direction | Purpose |
|---|---|---|
| `GET_INFO` | app→radio | firmware version, board, uptime, packet-store stats |
| `SET_RADIO` | app→radio | freq/bw/sf/cr/power/region preset |
| `GET_RADIO` | radio→app | current settings (for reconnecting clients) |
| `SEND_PACKET` | app→radio | raw packet bytes to key on-air (also how the app-driven repeater re-floods) |
| `FETCH_PACKETS (since_id)` | app→radio | stream stored packets: `[id][uptime_ms][rssi][snr][raw bytes]` |
| `FETCH_ACK (id)` | radio→app | per-store-id delivery/rx result for sends |
| `PURGE_STORE` | app→radio | clear packet store (Advanced) |

The radio acknowledges each with a small frame; framing/keepalive spec is in
[04-firmware](04-firmware.md) §4. Mesh-layer semantics (time correlation, ACK
logic) are computed in the app:

- **Packet timestamps:** radio stores `uptime_ms` per packet; app sends its
  current `uptime_ms` clock anchor at connect (`GET_INFO`) and maps
  `packet_time = app_now - (app_uptime_anchor - radio_packet_uptime)`.
  Long disconnects are handled by anchoring on each connect + periodic
  re-anchor; a radio RTC is a nice-to-have, not required.

## 6. App-level conventions layered on GRP_DATA (best-effort, app-to-app)

To satisfy UX needs without breaking MeshCore interop, MeshHop defines
optional GRP_DATA sub-messages (ignored by other clients):

- **Reactions:** `(msg_mac, emoji)` targeted at a specific message. UI covers
  for non-MeshHop peers (see 07 §Reactions).
- **Reply threading hints, read receipts (optional off by default),
  typing (off by default).** All are send-rate-limited and never retransmitted
  with the retry schedule.

## 7. Message metadata ("sub info") shown under messages

The app persists, per received message: uptime-mapped time, **SNR/RSSI at our
radio**, hop count (from path bytes when present), region/preset name, packet
tag, and ACK-derived relay info. Message row UI (see 07) shows a compact
status line; long-press → *Message details* shows everything. Delivery states
communicate the product requirement: the user must be able to see that at
least one repeater heard their message.

## 8. Regions = radio presets, nothing more

A "region" is a named bundle of RF settings (frequency, BW, SF, CR, power) plus
the public channel key variant used there — exactly the stupid-simple version
requested. Region list ships in-app (and via a QR/link import for custom
regions); choosing a region during onboarding configures the radio. No
region-specific mesh behavior exists in the protocol layer.

## 9. Interop test vectors

Golden vectors live in `meshhop-app/core-protocol/src/test/resources/vectors/`:
encode/decode round-trips for every payload type, cross-checked against
MeshCore firmware behavior on the bench (see 09). Vector rule: path byte
length must never be treated as hop count — hop count derives from
`path_bytes ÷ hash_size` (11 §2.3).
