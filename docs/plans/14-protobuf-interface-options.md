# 14 — Protobuf device interface (adopted design)

*Implemented, September 2026 — `protobufs/meshpigeon/*.proto` is the shipped
spec; see `docs/radio-protocol.md`.*

Status: **adopted** — protobuf payloads over the existing transports. This
plan records the decision, the research behind it, and the wire design. It
**supersedes the wire encoding in
[13-device-settings-and-interface-v2.md](13-device-settings-and-interface-v2.md)
§2–§12** (TLV registry, tag tables, hand-rolled versioning policy); every
*semantic* decision in plan 13 — settings defaults, atomicity, the auth model,
capabilities, status fields, factory reset, and the RAM-only packet store
(§16) — remains normative and carries over unchanged.

**Design in one line:** the COBS+CRC framing and all four transports stay
exactly as they are; the *payload* inside each frame becomes a single
serialized protobuf message whose `oneof` enumerates every operation; the
`.proto` files are the distributable, multi-language specification.

---

## 1. Why protobuf beats the custom payload format

Plan 13's TLV design was, honestly, hand-reimplementing protobuf: tag–length–
value fields, a hand-maintained tag registry, a hand-written skip-unknown-tags
rule, a hand-written versioning policy. Protobuf is that exact mechanism,
standardized, battle-tested, and — the part we cannot replicate — **codegen'd
into every language the app or a desktop tool might use.**

- **Spec distribution.** The `.proto` files *are* the specification. A Python
  bench script, the Android app, a future iOS app: all generate from the same
  file. Plan 13's spec was a Markdown contract humans re-implemented per
  language.
- **Evolution for free.** Adding a field or a `oneof` variant is backwards
  compatible by definition — every protobuf decoder on earth skips unknown
  fields. No registry to curate, no "major bump" ceremonies beyond an integer
  we report.
- **Operations in the spec.** The `oneof` members *are* the operation
  enumeration. A client author reads the proto and knows every verb.
- **Tooling.** `protoc --decode_raw` debugs any message from any transport
  today, before any client exists.
- **The cost is small and one-time:** a pinned nanopb generator (~2–5 KB
  flash, no malloc, static structs — see §4) and a codegen step in the build,
  mitigated by committing the generated `.pb.c/.pb.h` (regen script + CI
  freshness check), so PlatformIO builds stay dependency-free.

## 2. What this is *not*: deliberately not Meshtastic

Meshtastic (`../firmware`) was research material, not a template. Verified
facts from their implementation (kept here as the record): protos in a
submodule; nanopb codegen into the tree; **no gRPC anywhere** — operations are
`ToRadio`/`FromRadio` envelope `oneof`s; serial/TCP framing is a `0x94C3`
magic + length header; version/capabilities via a `DeviceMetadata` handshake
message; TCP is persistent per client (multi-client via per-connection API
instances); a separate UDP transport exists for connectionless use.

What we take: the *architectural pattern* (envelope messages, nanopb,
protos-as-spec). What we reject:

- **No submodule** — `protobufs/` lives in this repo. If a second consumer
  repo ever wants it standalone, extraction is mechanical.
- **No `0x94C3` header** — it exists because Meshtastic mixes debug text into
  serial streams; our COBS framing is already uniform across USB/BLE/TCP,
  self-resynchronizing, and CRC-checked. COBS stays.
- **No `want_config_id` handshake state machine** — our `FETCH_PACKETS`
  cursor model already does replay cleanly; a connect triggers an ordinary
  fetch.
- **No `fromNum` polling characteristic** — we push frames to sinks; the
  sink/broadcast model already works.
- **No Meshtastic naming** — our own package, message, and field names.

## 3. The wire design

### 3.1 Framing (unchanged)

```
wire frame :=  COBS(serialized_message)  0x00
```

The COBS layer, CRC16, `FrameReader`, `IFrameSink`, the sink registry and
broadcast are all untouched. The only change to `frame_build`/`on_frame` is
that the decoded payload is now an opaque serialized protobuf message instead
of `[cmd][nonce][status][...]` — those three fields move *inside* the
protobuf envelope (§3.2), so the `.proto` file is the complete wire contract.

Because the envelope carries its own correlation id and status (4–6 bytes
where the header spent 2), the frame payload cap rises from 220 to **512**
bytes — sized so a worst-case `PacketEntry` (255-byte raw packet, the Semtech
SX12xx silicon cap that covers everything MeshCore and Meshtastic put on the
air) plus entry metadata fits with headroom, matching the BLE-friendly 512
Meshtastic settled on for the same reason. COBS overhead stays ≤ 2 bytes; BLE
20-byte chunking is unaffected.

> **Authoritative schema.** The sketches below are illustrative; the real,
> field-numbered schema now lives in this repo and is the contract:
>
> - `protobufs/meshpigeon/envelope.proto` — `ClientToRadio` /
>   `RadioToClient` envelopes, `Error` codes, `Ok`/`Ping`/`Pong`, and the
>   normative evolution policy (additive-only, reserved-field rules).
> - `protobufs/meshpigeon/device.proto` — device info, device settings,
>   status, auth, reset/boot messages.
> - `protobufs/meshpigeon/radio.proto` — radio tuning and packet-store
>   messages.
> - `protobufs/meshpigeon/*.options` — nanopb static-buffer caps mirroring
>   the documented field limits.
> - `scripts/regen-protos.sh` + committed generated output;
>   `buf lint` / `buf breaking` guard schema quality and the evolution
>   policy mechanically in CI.
>
> Refinements the schema made versus plan 13's tables, all semantics-preserving:
> the PIN is **write-only on the wire** (reads return `pin_set`, never the
> value — plan 13 §6's read model allowed authed clients to read it, which was
> needlessly leaky); radio bandwidth is named `bandwidth_hz` (plain Hz, not
> the v1 blob's 0.01-kHz units); IPv4 status is 4 network-order bytes.

### 3.2 Envelope and correlation

```protobuf
syntax = "proto3";
package meshpigeon;

// Client -> device. One serialized message per COBS frame.
message ClientToRadio {
  uint32 id = 1;          // correlation id, echoed by the response
  oneof body {
    Ping ping = 2;
    GetDeviceInfo get_device_info = 3;
    GetRadioSettings get_radio_settings = 4;
    SetRadioSettings set_radio_settings = 5;
    SendPacket send_packet = 6;
    FetchPackets fetch_packets = 7;
    PurgeStore purge_store = 8;
    GetDeviceSettings get_device_settings = 9;
    SetDeviceSettings set_device_settings = 10;
    GetStatus get_status = 11;
    Auth auth = 12;
    Reboot reboot = 13;
    FactoryReset factory_reset = 14;
    Bootloader bootloader = 15;
  }
}

// Device -> client. Responses echo the request's id; async pushes use id = 0.
message RadioToClient {
  uint32 id = 1;
  oneof body {
    Pong pong = 2;
    DeviceInfo device_info = 3;
    RadioSettings radio_settings = 4;      // response & RadioChanged push
    PacketAccepted packet_accepted = 5;
    PacketEntry packet_entry = 6;          // live push & fetch stream item
    FetchEnd fetch_end = 7;
    TxResult tx_result = 8;
    DeviceSettings device_settings = 9;    // response & DeviceSettingsChanged
    Status status = 10;                    // response & DeviceStatusChanged
    Ok ok = 11;                            // auth/reboot/factory_reset/purge acks
    Error error = 12;
  }
}

message Error {
  Code code = 1;          // BAD_CMD, BAD_PAYLOAD, BUSY, TX_FAILED, NO_RADIO,
  string message = 2;     //   NOT_SUPPORTED, AUTH_REQUIRED (plan 13 semantics)
}
```

(Streaming `FETCH_PACKETS` works exactly as today: one `RadioToClient.packet_entry`
frame per entry, all carrying the request's id, terminated by a
`RadioToClient.fetch_end`.)

### 3.3 Version & capabilities

`GetDeviceInfo` returns `DeviceInfo`:

```protobuf
message DeviceInfo {
  uint32 spec_version = 1;      // the version THIS proto file set implements
  string fw_version = 2;        // "0.2.0"
  string board_name = 3;
  repeated Capability capabilities = 4; // plan 13 §5, as enum values (see §6)
  uint64 uptime_ms = 5;         // monotonic 64-bit, never wraps (see §6)
  uint32 boot_count = 6;        // informational: real boots, persisted
  uint32 store_count = 7;
  uint32 store_capacity = 8;
  uint32 store_dropped = 9;
  uint32 oldest_seq = 10;
  uint32 radio_config_epoch = 11;
  uint32 battery_mv = 12;       // 0xFFFF = unknown
  bool radio_ok = 13;
  bool auth_required = 14;      // plan 13 §8: PIN set, client unauthenticated
}
```

`spec_version` is a plain integer under our control: start at 2 (continuing
plan 13's numbering), increment only for whole-spec breaks. Between breaks,
additive proto changes need no bump — clients gate on the fields they know.

### 3.4 Semantics (all inherited from plan 13 — unchanged)

First-owner lock on `SET_RADIO`; atomic validated `SET_DEVICE_SETTINGS` with
`NOT_SUPPORTED` for capability-gated fields on incapable boards; protocol-level
`Auth` with per-connection state and rate limiting; effective-default device
name (empty stored name = `MeshPigeon-XXXX` from the MAC, live GAP rename);
factory reset = settings defaults **and** RAM store `clear()`; status
broadcast on Wi-Fi state transitions. Nothing in this section re-opens those
decisions; the `.proto` merely re-expresses their payload shapes.

## 4. Memory budget (the ring-buffer question, answered)

**The packet store is untouched — byte for byte.** The ring keeps raw on-air
entries (`[seq][uptime][rssi][snr][flags][len][raw]`, 12 + len bytes). That is
an *internal storage format*; protobuf exists only at the interface, and each
message is encoded transiently into the frame buffer that already exists at
delivery/fetch time. No entry is ever re-encoded for storage; retention
capacity (64 KB nRF52 / 256 KB xiao_wio budgets) is unchanged to the byte.

| Cost | Size | Notes |
|---|---|---|
| nanopb runtime | ~2–5 KB flash | no malloc, no heap |
| Generated `.pb.c/.pb.h` | ~3–8 KB flash | all messages, one file pair |
| Encode/decode structs | stack-local per handler, ≤ ~250 B | largest = `DeviceSettings` (63 B password + 32 B SSID + fixed fields) |
| Frame payload cap | 220 → 512 B | fits a 255-byte raw packet entry + envelope overhead; `FrameReader` wire buffer grows a few bytes |

No board is near its flash ceiling; the nRF52 boards gain a few KB of the ~40 KB
of headroom their images have, the ESP32-S3 boards effectively nothing
measurable. Per-packet *air-over-interface* cost grows ~2–6 bytes/entry versus
the hand-packed layout (proto tags + varints) — negligible on BLE notify and
TCP alike, and fetch streaming still sends one entry per frame, so there is no
batching buffer to grow.

## 5. Next steps (implementation order)

1. ~~Write `protobufs/meshpigeon/radio.proto`~~ — **done**: the full schema
   (envelope, device, radio) is in `protobufs/meshpigeon/` with nanopb
   `.options` caps, `scripts/regen-protos.sh`, and `buf.yaml` for CI lint/
   breaking checks.
2. Pin nanopb; add `scripts/regen-protos.sh`; commit generated output into
   `lib/meshpigeon-core/src/generated/` (or `src/`); CI checks freshness.
3. Rework `command_processor` internals: decode envelope, dispatch on
   `oneof`, handlers encode responses. Semantics code paths (store, settings,
   auth, locks) are untouched — only payload encode/decode changes.
4. Raise `MESHPIGEON_MAX_FRAME_PAYLOAD` to 256; adjust sim and tests.
5. Host tests: every message round-trips; unknown-field skipping (encode a
   future field, decode with current schema); envelope id correlation;
   fetch-stream framing.
6. Rewrite `docs/radio-protocol.md` v2 as: the framing chapter (COBS/CRC,
   unchanged) + "the spec is `protobufs/meshpigeon/radio.proto`, spec_version
   2" + the semantics chapters inherited from plan 13.
7. Sim: speaks the same envelope (it already speaks COBS over TCP, multi-
   client).
8. Plan 13's §3–§12: mark superseded by this plan; §16 (RAM-only store) and
   the semantics sections stay authoritative.

---

## 6. Decision log (client review)

Decisions made during schema review, applied to `protobufs/meshpigeon/`:

1. **Capabilities are a `repeated Capability` enum, not a bitmask.** Same
   skip-unknown evolution property as fields, self-documenting, and never
   exhausts 32 bits. Clients treat unknown values as "ignore this entry";
   `ERROR_CODE_NOT_SUPPORTED` rejections remain the backstop for writes.
2. **The PIN is always set; the default is the public `"0000"`.** No
   "absent PIN" state exists in the spec at all — the device ships with the
   classic Bluetooth default, knowable without documentation, and the user
   may replace it (4–8 digits). `DeviceSettings.pin_is_default` reports
   whether it is still the default; the value itself stays write-only. The
   out-of-the-box experience is unchanged: the app just AUTHs with `0000`.
3. **No `region` in `RadioSettings`.** The tuning surface is exactly
   freq / BW / SF / CR / TX power (+ the firmware-owned `config_epoch`).
   The v1 blob's region-preset concept is gone; the safe first-boot default
   is a plain frequency/band/SF/CR/power set.
4. **Raw packet max is 255 bytes** (was 200) — the Semtech SX12xx silicon
   cap, verified against both stacks: Meshtastic's `MAX_LORA_PAYLOAD_LEN =
   255`, MeshCore's `MAX_PACKET_PAYLOAD = 184`. The frame payload cap is
   **512** so a worst-case entry + envelope fits with headroom.
5. **`PacketEntry.origin` is an enum (`ORIGIN_SENT` / `ORIGIN_RECEIVED`),
   not a flags bitmask.** The bitmask was a space optimization that made
   sense on-air; this interface only ever crosses USB/BLE/Wi-Fi where bytes
   are not the constraint, so explicit self-documenting fields win. New
   origins can be added as enum values later.
6. **Time is a 64-bit monotonic uptime — the client needs zero inside
   knowledge.** An intermediate design (`session` epochs stamped on entries,
   clients maintaining per-session anchors) worked but pushed wrap reasoning
   onto every client. Final design: the firmware exposes `uptime_ms` as a
   **uint64** — internally the 32-bit `millis()` counter plus a RAM rollover
   counter the `UptimeClock` increments when it observes the wrap (polled
   every loop pass, so a wrap is never missed) — and `PacketEntry.uptime_ms`
   carries the same 64-bit clock. `uint64` ms cannot wrap in practice
   (~584 million years), so anchoring is one pairing per connection and
   every packet places with plain arithmetic. Consequences:
   - the rollover counter is **RAM-only** — no flash writes for time, ever;
   - `boot_count` stays as a persisted, purely informational counter of real
     boots (it already costs one flash write per boot, which main does
     today); it plays no role in timestamping;
   - a reboot legitimately restarts the clock and empties the RAM store;
     clients re-anchor trivially (uptime went down) and refetch from the new
     `oldest_seq` baseline;
   - internal cost: `StoredPacket`'s uptime grows u32 → u64 (entry header
     12 → 16 bytes; ~0.3% worst-case capacity impact on the ring), and
     `UptimeClock` gains the rollover counter + u64 accessors.
7. **`DeviceSettings.pin_is_default` removed** — the client tracks what it
   set in its own database; the firmware does not mirror UI state. Field
   number 2 is `reserved`, per the evolution policy.
