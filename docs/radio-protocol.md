# MeshPigeon Radio Protocol v1

The public, versioned contract between the MeshPigeon app and a MeshPigeon radio.
The radio is **dumb on purpose**: it receives packets into the largest memory
it can hold, stamps each with uptime, keys up on demand, and persists its
radio settings. It speaks **no mesh protocol** and holds **no keys**.

This document is normative for `PROTOCOL_VERSION = 1`. The firmware answers
`GET_INFO` with this version byte; the app must refuse to talk to radios it
cannot understand (and vice versa: unknown protocol version → refuse).

- Framing, commands, and payloads are identical on every transport
  (USB CDC, BLE, Wi-Fi TCP).
- All multi-byte integers are **little-endian**.
- The firmware never parses packet payloads — raw bytes are all it keeps.

## 1. Transports

| Transport | Bearer | Notes |
|---|---|---|
| USB CDC | virtual COM / serial | 115200 8N1 (line coding ignored); the primary bench/debug path |
| BLE | Nordic UART Service (`6E400001-B5A3-F393-E0A9-E50E24DCCA9E`) | write-to-device char `6E400002-…`, notify-from-device char `6E400003-…`; multiple centrals supported (v1 target: ≥ 3) |
| Wi-Fi TCP | raw TCP server | planned for ESP32 Wi-Fi boards (stretch) |

BLE notify chunks frames at ≤ 20 bytes so pre-MTU-exchange clients work.

## 2. Framing

```
wire frame   :=  COBS(decoded_frame)  0x00
decoded      :=  cmd(1)  nonce(1)  status(1)  payload(0..220)  crc16(2)
```

- **COBS**: consistent overhead byte stuffing; the 0x00 byte terminates each
  frame. Frames may be split across USB packets / BLE writes / TCP segments.
- **CRC-16/CCITT-FALSE** (poly 0x1021, init 0xFFFF) over
  `cmd + nonce + status + payload`, appended little-endian.
- **cmd**: one byte (§3). **nonce**: request correlation — responses carry
  the request's nonce; async frames use nonce 0.
- **status**: 0x00 = OK. Requests must use 0x00. Non-zero in a response means
  an error (§4) and `payload` carries a short human-readable hint.
- A frame with a bad CRC or malformed COBS body is dropped silently. Streams
  of garbage between delimiters self-heal at the next frame.

### 2.1 Entry payload (packet store records)

Used by `RX_PACKET`, `FETCH_PACKETS` streaming entries, and `TX_RESULT`:

```
[seq:u32][uptime_ms:u32][rssi:i8][snr:i8][flags:1][len:1][raw:len]
```

- `seq` — monotonic store id, never reused across store wraps or purges.
- `uptime_ms` — radio uptime at rx/tx time (see §6).
- `flags` — bit 0 = sent by us (TX), bit 1 = received.

## 3. Commands

Requests are `cmd + nonce + status 0x00 + payload`; responses use the same
`cmd` and nonce.

| cmd | Name | Request payload | Response |
|---|---|---|---|
| 0x01 | `PING` | arbitrary | status OK, payload echoed |
| 0x02 | `GET_INFO` | empty | 49-byte info blob (§5) |
| 0x03 | `GET_RADIO` | empty | 17-byte settings blob (§7) |
| 0x04 | `SET_RADIO` | 17-byte settings blob | OK + 17-byte settings (post-bump) |
| 0x05 | `SEND_PACKET` | `[len:1][raw:len]` (len ≤ 200) | OK + `[seq:u32]`; async `TX_RESULT` follows |
| 0x06 | `FETCH_PACKETS` | `[since_seq:u32][max_count:u16]` | stream: up to `max_count` `RX_PACKET` frames (nonce = request's) + one `FETCH_END [count:u16]` |
| 0x07 | `PURGE_STORE` | empty | OK (Advanced feature; wipes history) |
| 0x08 | `BOOTLOADER` | empty | OK, then the device reboots for update (in-app flashing, phase 2) |

### Async frames (radio → host, nonce 0)

| cmd | Name | Payload |
|---|---|---|
| 0x10 | `RX_PACKET` | entry payload (§2.1) — live push while connected |
| 0x11 | `FETCH_END` | `[count:u16]` — terminates a fetch stream |
| 0x12 | `TX_RESULT` | `[seq:u32][status:1]` — per-store-id tx outcome |
| 0x13 | `RADIO_CHANGED` | `[epoch:u32][settings blob:17]` — another client re-tuned |

## 4. Status codes

| Code | Meaning |
|---|---|
| 0x00 | OK |
| 0x01 | `ERR_BAD_CMD` — unknown command |
| 0x02 | `ERR_BAD_PAYLOAD` — wrong size/format |
| 0x03 | `ERR_BAD_CRC` — (never emitted; bad frames are dropped) reserved |
| 0x04 | `ERR_BUSY` — TX in flight, or first-owner lock (§7.1) |
| 0x05 | `ERR_TX_FAILED` — radio refused/failed to transmit |
| 0x06 | `ERR_NO_RADIO` — radio init failed |

## 5. `GET_INFO` response (49 bytes)

```
0    proto version (1)
1    fw version major
2    fw version minor
3    board name, up to 16 bytes, zero-padded ("XIAO WIO", "HELTEC V3", "T114", "T1000-E", "SIM")
19   uptime_ms      u32
23   boot_count     u32   (increments each boot; see §6)
27   store count    u32   (packets currently retained)
31   store capacity u32   (store byte budget — entries are variable-length,
                          so packet capacity depends on packet sizes)
35   store dropped  u32   (packets evicted by overflow)
39   oldest seq     u32   (first retained seq, or the next seq to be
                          assigned when the store is empty)
43   config epoch   u32   (see §7.1)
47   battery mV     u16   (0xFFFF = unknown; device status, not mesh telemetry)
```

## 6. Time

The radio keeps a free-running 32-bit `uptime_ms` (wraps ~49.7 days). It has
no RTC and no wall clock — **time correlation is the app's job**: on connect
the app anchors radio uptime to app time via `GET_INFO` and re-anchors
periodically (every 15 min); `boot_count` in settings storage disambiguates
uptime wrap for long-running radios. Message timestamps are computed
app-side and are never stored on the radio.

## 7. Radio settings (17 bytes)

```
0    blob version (1)
1    region preset id (0 = UNSET / app default)
2    freq_hz        u32
6    bandwidth      u16   (0.01 kHz units; 12500 = 125.00 kHz)
8    spreading factor  u8 (5..12)
9    coding rate    u8    (5..8 meaning 4/5..4/8)
10   tx power dBm   u8
11   config epoch   u32
15   crc16 over bytes 0..14 (little-endian)
```

- `SET_RADIO` **persists immediately** (NVS/flash) and applies; a radio that
  reboots alone resumes listening with these settings. First boot ships a
  safe default and does not persist until an app tunes it.
- The radio **ignores the request's epoch value** and bumps its own epoch by
  1 on every accepted `SET_RADIO`; all connected clients except the issuer
  receive `RADIO_CHANGED`.

### 7.1 First-owner lock

During the **first 5 minutes after boot**, only the *first* `SET_RADIO` is
honored; further attempts get `ERR_BUSY`. This gives the first-connected
phone an uncontended tuning window (multi-client sharing, app plan §5.3).
Clients handle `ERR_BUSY` by reading `GET_RADIO` and offering the user
"[Use radio's] / [Apply mine]" after the window.

## 8. Semantics the radio does NOT have

Deliberately, per the product's guiding principles:

- No packet parsing, no deduplication, no repeat/rebroadcast logic.
- No ACK logic, no retry logic — the app listens for on-air ACKs through
  the same packet store and owns the retry schedule.
- No keys, no identity, no adverts. A stolen radio leaks nothing.
- No mesh-telemetry. The only status fields are device-level (§5 battery).

## 9. Reference

- Firmware: `github.com/jhuebert/meshpigeon-firmware` (this repo)
- App implementation: `meshpigeon-app :core-transport`
- Original design: `docs/plans/03-mesh-protocol.md` (§5) and
  `docs/plans/04-firmware.md` (§3, §4) in the founding plan set.
