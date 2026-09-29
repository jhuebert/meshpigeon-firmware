# MeshPigeon Radio Protocol v2

The public, versioned contract between the MeshPigeon app and a MeshPigeon radio.
The radio is **dumb on purpose**: it receives packets into the largest memory
it can hold, stamps each with uptime, keys up on demand, and persists its
radio settings. It speaks **no mesh protocol** and holds **no keys**.

## 0. The spec is the `.proto` files

```
protobufs/meshpigeon/envelope.proto   envelopes, Error, Ping/Pong
protobufs/meshpigeon/device.proto     device info, settings, auth, status
protobufs/meshpigeon/radio.proto      radio tuning, packet store operations
```

Those three files **are** the wire contract: field numbers, oneof variants,
enum values and the per-field size limits (the `*.options` files that cap
every string/bytes field) are normative. This document is their prose
companion: framing mechanics, the semantics behind the fields, and the
evolution policy. Where the two disagree, the `.proto` files win.

`DeviceInfo.spec_version` is **2** for this document. A client must refuse a
radio whose `spec_version` it does not implement; the firmware never branches
on the client's version — it only reports its own.

- The wire payload of every frame is a serialized protobuf envelope
  (`ClientToRadio` or `RadioToClient`), identical on every transport.
- The firmware never parses packet payloads — raw bytes are all it keeps.
- The generated C sources are committed under
  `lib/meshpigeon-core/src/generated/` and the nanopb runtime is vendored in
  `lib/meshpigeon-core/src/nanopb/`, so a build needs nothing but PlatformIO.
  Regenerate with `scripts/regen-protos.sh` (CI fails on stale output).

## 1. Transports

| Transport | Bearer | Notes |
|---|---|---|
| USB CDC | virtual COM / serial | 115200 8N1 (line coding ignored); the primary bench/debug path |
| BLE | Nordic UART Service (`6E400001-B5A3-F393-E0A9-E50E24DCCA9E`) | write-to-device char `6E400002-…`, notify-from-device char `6E400003-…`; multiple centrals on ESP32, single central on nRF52 |
| Wi-Fi TCP | raw TCP server, default port **5000** | multi-client (≤ 4), ESP32 boards only (§7); mDNS `_meshpigeon._tcp` |

BLE notify chunks frames at ≤ 20 bytes so pre-MTU-exchange clients work.

## 2. Framing

```
wire frame   :=  COBS( envelope ‖ crc16 )  0x00
```

- **COBS**: consistent overhead byte stuffing; the 0x00 byte terminates each
  frame. Frames may be split across USB packets / BLE writes / TCP segments.
- **CRC-16/CCITT-FALSE** (poly 0x1021, init 0xFFFF) over the serialized
  envelope, appended little-endian *before* the COBS pass.
- A frame with a bad CRC or a malformed COBS body is dropped silently; the
  next frame in the stream decodes normally.
- The receiver hands the decoded envelope (CRC verified and stripped) to the
  command processor. A maximum-size frame payload is **512 bytes**; a raw
  on-air packet is ≤ **255 bytes** (the Semtech SX12xx silicon cap).

Since v2 there is no `[cmd][nonce][status]` frame header. Correlation, the
operation and the error code all live *inside* the envelope:

- `ClientToRadio.id` — a client-chosen non-zero correlation id. Every response
  echoes it. An envelope that decodes but carries id 0 (or fails to decode at
  all) is dropped without a reply: there is no id to correlate an error to.
- Async pushes use `id = 0`.

## 3. Operations

The `body` oneofs in `envelope.proto` *are* the operation list. Each operation
has its own message, even the empty ones, so requests can gain fields later
without breaking older firmware (which skips what it does not know).

| Request | Response | Notes |
|---|---|---|
| `Ping` | `Pong` | echoes the payload bytes |
| `GetDeviceInfo` | `DeviceInfo` | identity, spec version, capabilities, store stats, health (§4) |
| `GetRadioSettings` | `RadioSettings` | current tuning |
| `SetRadioSettings` | `RadioSettings` | applies, bumps `config_epoch`, persists (§6) |
| `SendPacket` | `PacketAccepted` | then an async `TxResult` (§5) |
| `FetchPackets` | a `PacketEntry` per packet, then `FetchEnd` | stream, every frame echoes the request id |
| `PurgeStore` | `Ok` | history only; settings untouched |
| `GetDeviceSettings` | `DeviceSettings` | requires auth (§8) |
| `SetDeviceSettings` | `DeviceSettings` | full post-write read model (§8) |
| `GetStatus` | `Status` | **no auth** — this is how a BLE client learns what Wi-Fi is doing (§9) |
| `Auth` | `Ok` | unlocks *this* connection (§8) |
| `Reboot` | `Ok`, then restart | requires auth |
| `FactoryReset` | `Ok`, then wipe + restart | requires auth (§8) |
| `Bootloader` | `Ok`, then restart into DFU | **never** gated — flashing must work on a locked node |

Async pushes (`id = 0`), broadcast to every connected client:

| Message | Sent when |
|---|---|
| `PacketEntry` | a packet was received on the air (live push, same shape as a fetch entry) |
| `TxResult` | a `SendPacket` transmission finished |
| `RadioSettings` | another client re-tuned the radio (sent to everyone *except* the issuer) |
| `DeviceSettings` | another client changed device settings (everyone except the issuer) |
| `Status` | the Wi-Fi state changed (§9) |

## 4. Errors

`Error.code` is normative in `envelope.proto`; `Error.message` is a short
human-readable hint, explicitly not a contract.

| Code | Meaning |
|---|---|
| `ERROR_CODE_BAD_COMMAND` | unknown operation (radio predates the oneof variant, or a bug) |
| `ERROR_CODE_BAD_PAYLOAD` | wrong size/format/validation — rejected atomically, nothing applied |
| `ERROR_CODE_BUSY` | TX in flight, or the first-owner lock is active (§6) |
| `ERROR_CODE_TX_FAILED` | the radio refused or failed the transmission |
| `ERROR_CODE_NO_RADIO` | radio init failed; the device answers but cannot use the air |
| `ERROR_CODE_NOT_SUPPORTED` | feature absent on this board (capability-gated, §8) |
| `ERROR_CODE_AUTH_REQUIRED` | a PIN is set and this connection has not authenticated (§8) |

## 5. `DeviceInfo`

| Field | Meaning |
|---|---|
| `spec_version` | 2 |
| `fw_version`, `board_name` | strings ("XIAO WIO", "HELTEC V3", "T114", "T1000-E", "SIM") |
| `capabilities` | repeated enum: `WIFI_STA`, `BATTERY`, `BLE`, `USB_CDC` — compile-time facts, never probed |
| `uptime_ms` | monotonic 64-bit uptime (§5.1) |
| `boot_count` | real boots, persisted; informational only |
| `store` | `count`, `capacity_bytes`, `dropped`, `oldest_seq` |
| `radio_config_epoch` | bumped on every accepted `SetRadioSettings` |
| `battery_mv` | 0xFFFF = unknown (device status, not mesh telemetry) |
| `radio_ok` | false when the radio failed to init or apply |
| `auth_required` | true when a PIN is set and *this* connection is not authenticated |

### 5.1 Time

The radio keeps a monotonic **64-bit** `uptime_ms`: the 32-bit `millis()`
counter plus a RAM rollover count. It has no RTC and no wall clock — **time
correlation is the app's job**: on connect the app anchors radio uptime to app
time via `GetDeviceInfo` and re-anchors periodically. `PacketEntry.uptime_ms`
comes from the same clock, so every timestamp is plain arithmetic on the
client side — no rollover reasoning anywhere.

The rollover counter lives in RAM only (no flash writes for time, ever), so a
reboot legitimately restarts the clock and empties the store. Clients
re-anchor; that is the whole protocol for reboot detection.

## 6. Radio settings

`RadioSettings` speaks plain engineering units: `freq_hz` and `bandwidth_hz`
in Hz, `sf` 5..12, `cr` 5..8 (meaning 4/5..4/8), `power_dbm`. The v1 region
preset is **gone** — the app owns channels and legality; the firmware just
tunes what it is told.

- `SetRadioSettings` **persists immediately** (NVS / internal FS) and applies.
  A radio that reboots alone resumes listening with these settings. First
  boot ships a safe default and does not persist until an app tunes it.
- The firmware **ignores the request's `config_epoch`** and bumps its own by 1
  on every accepted change; all connected clients except the issuer get the
  async `RadioSettings` push, then the issuer gets the post-bump settings as
  its response.
- Out-of-range values (zero frequency/bandwidth, SF or CR out of range) are
  rejected with `ERROR_CODE_BAD_PAYLOAD`; a radio that refuses the applied
  settings answers `ERROR_CODE_TX_FAILED`.

### 6.1 First-owner lock

During the **first 5 minutes after boot**, only the *first*
`SetRadioSettings` is honored; further attempts get `ERROR_CODE_BUSY`. This
gives the first-connected phone an uncontended tuning window for
multi-client sharing. Clients handle `ERROR_CODE_BUSY` by reading
`GetRadioSettings` and offering the user "[Use radio's] / [Apply mine]" after
the window. Device settings have no such lock: identity is a once-per-device
decision and last-writer-wins is self-healing.

## 7. Wi-Fi station (ESP32 boards only)

- **Lifecycle.** Station mode only, one network, DHCP, applied from the
  device settings. Enable → connect; disable → disconnect and close the
  server; credential change while enabled → reconnect. The TCP port can
  change while up, which rebinds the listener. A pigeon with Wi-Fi enabled
  comes back on the network after a reboot with no app attached.
- **Backoff.** Retries run on an exponential backoff — 5 s doubling to a
  2 min cap, forever. A wrong password is not fatal: it surfaces as
  `WIFI_STATE_AUTH_FAIL` in `Status`, and the user may have rotated the
  password. No "disable after N failures" policy exists.
- **Multi-client TCP**, exactly like the ESP32 BLE sink: one frame reader and
  one sink per socket (≤ 4, the size of the sink registry), broadcast to all
  of them. Default port 5000 (the convention MeshCore's desktop tooling
  standardized on), overridable with `wifi_port`.
- **mDNS.** The device advertises `<hostname>.local` with a `_meshpigeon._tcp`
  service on the current port, so desktop tooling finds the pigeon without
  typing an IP. The hostname shares the derived device-name suffix
  (`meshpigeon-A3F2`).
- **No TLS, no cloud, no outbound connections.** The firmware listens; it
  never dials.

## 8. Device settings, the PIN, and the name

`DeviceSettings` is the device-level read model: the name, the Wi-Fi station
settings, and the port. The **PIN is write-only on the wire** — no field of
`DeviceSettingsMessage` carries it, and none ever will; the app remembers what
it set. (`SetDeviceSettings.pin` uses field number 2, which is why the
read model reserves that number.)

### 8.1 `SetDeviceSettings` semantics

- **Atomic.** Every present field is validated first; one bad field rejects
  the whole request with `ERROR_CODE_BAD_PAYLOAD` and **nothing** is applied.
  Partial application invites "which half saved?" UI bugs, and we never want
  a user to believe a setting took when the board can never honor it.
- **Validated.** name ≤ 20 bytes; PIN 4–8 ASCII digits; SSID ≤ 32; password
  ≤ 63 (the WPA2/3 limit); port ≠ 0.
- **Capability-gated.** Any Wi-Fi field on a board without the `WIFI_STA`
  capability → `ERROR_CODE_NOT_SUPPORTED`, nothing applied. The app hides
  those fields using `DeviceInfo.capabilities`; the firmware rejects as a
  backstop so a stale client cannot leave a dead setting behind.
- **Persists immediately** and applies live: a name change re-advertises, a
  PIN change re-gates immediately, Wi-Fi settings connect/disconnect.
- **No first-owner lock** (§6.1).
- Accepted writes broadcast the full post-write `DeviceSettings` to all
  *other* clients, then answer the issuer with the same read model.

### 8.2 The PIN

The node holds no identity and no keys, so the PIN exists purely to stop
*casual* use: a stranger scanning Bluetooth, or someone on the same LAN who
discovered the TCP port. It is a protocol-level gate — not BLE pairing —
because it must work identically on USB CDC, BLE and Wi-Fi TCP, none of which
have a shared pairing concept.

- **Always set; the default is the public `"0000"`**, exactly like every
  Bluetooth device's default PIN. There is no "no PIN" state: with the
  default in place the node ships open, and a user-chosen replacement is what
  actually protects it. A custom PIN is 4–8 ASCII digits.
- **Exempt operations** (reachable without authenticating): `ping`,
  `get_device_info`, `get_status`, `auth`, `bootloader`. Everything else
  answers `ERROR_CODE_AUTH_REQUIRED`.
- **Per-connection state.** Authenticating on BLE says nothing about the TCP
  socket someone else opened; each connection holds its own flag.
- **Rate limit.** The first 3 failed attempts are free. From the 4th on, a
  1-second penalty window opens, and during it every attempt — even with the
  correct PIN — is rejected unevaluated. Changing the PIN clears the budget.
- A static PIN over plaintext frames stops casual misuse, not a determined
  attacker with a sniffer. That is the right level for a device whose worst
  case leak is packets the *sender* already encrypted.

### 8.3 Name & BLE advertising

The **effective name** is the stored name if non-empty, else
`MeshPigeon-XXXX` where `XXXX` is the BLE address's two low bytes in hex — the
scheme v1 already advertises. The derived default is never persisted; the app
reads the effective name from `DeviceSettings` and prefills it. Writing an
empty name resets to the derived default. A name change renames the GAP
device and restarts advertising: live connections are unaffected, scanners
see the new name at the next advertising round.

## 9. Status

`GetStatus` is the snapshot; the async `Status` push fires **only on Wi-Fi
state transitions** (no RSSI churn).

| Field | Meaning |
|---|---|
| `wifi_state` | `OFF`, `CONNECTING`, `CONNECTED`, `AUTH_FAIL` (AP rejected us), `ERROR` (no AP / driver) |
| `wifi_ssid` | the SSID we are configured for |
| `wifi_ipv4` | 4 bytes, network byte order; empty when not connected |
| `wifi_port` | the port the TCP server listens on |
| `wifi_rssi` | dBm; **0 = unknown** (not connected) |
| `ble_clients` | connected BLE centrals |

## 10. Packet store

A byte-budgeted RAM ring: entries cost a 16-byte
header plus their exact payload length, and overflow evicts the oldest. A
packet larger than the whole pool is dropped and counted in `dropped`.

- `SendPacket` stores the packet with origin `SENT` and keys up. One TX at a
  time: a second concurrent send answers `ERROR_CODE_BUSY`; the app's outbox
  owns retry policy. The `TxResult` push reports the outcome later.
- Packets received on the air are stored with origin `RECEIVED` and pushed
  live to every connected client.
- `FetchPackets` streams every retained entry with `seq > since_seq`, oldest
  first, up to `max_count`, then `FetchEnd` with the delivered count.
  `since_seq` cursors stay valid across wraps and purges because sequence
  numbers are monotonic and eviction only removes from the oldest end.
- `PurgeStore` clears history only. `FactoryReset` clears settings **and**
  history, then wipes device-settings persistence and reboots: "as it
  shipped". The radio tuning and the boot count survive a factory reset.

## 11. What is persisted

The persistence inventory is **closed** — three things, ever:

| Item | ESP32 | nRF52 |
|---|---|---|
| radio settings | NVS `meshpigeon/radio` | `/radio.bin` |
| boot count | NVS `meshpigeon/boots` | `/boots.bin` |
| device settings (name, PIN, Wi-Fi) | NVS keys `name`, `pin`, `wifie`, `wifissid`, `wifipass`, `wifiport` | `/dev.bin` |

Nothing else is ever written: **no packet bytes, no timestamps, no counters.**
Power loss or reboot clears the packet history — that is a hardware property,
not a policy.

## 12. Semantics the radio does NOT have

Deliberately, per the product's guiding principles:

- No packet parsing, no deduplication, no repeat/rebroadcast logic.
- No ACK logic, no retry logic — the app listens for on-air ACKs through
  the same packet store and owns the retry schedule.
- No keys, no identity, no adverts. A stolen radio leaks nothing.
- No mesh telemetry. The only status fields are device-level.

## 13. Evolution policy (normative)

- **Only ever ADD** fields, oneof variants, or enum values.
- **Never renumber or reuse a field number**; removed ones become `reserved`.
- Unknown fields and unknown oneof variants are skipped by every protobuf
  decoder, so additive changes are backwards compatible by construction. A
  radio that predates a variant answers `ERROR_CODE_BAD_COMMAND`.
- **Additive proto change ≠ `spec_version` bump.** `spec_version` is reserved
  for whole-spec breaks: removing or renumbering a field, changing a field's
  width or meaning, or changing the framing itself. Every bump after 2 ships
  with a paragraph here justifying it.
- Reserved for later, capability-gated, and not implemented today: RTC
  wall-clock, Ethernet, LED/button behaviour. Rejected outright (it would
  give the firmware opinions): anything about packet content, identity, or
  keys.

## 14. Reference

- Firmware: `github.com/jhuebert/meshpigeon-firmware` (this repo)
- Schema: `protobufs/meshpigeon/*.proto` in this repo
- App implementation: `meshpigeon-app :core-transport`
- Working on the firmware: `AGENTS.md` (build, layout, conventions, traps)
- Principles: `GUIDING-PRINCIPLES.md`
