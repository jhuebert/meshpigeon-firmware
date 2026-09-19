# 04 — Radio Firmware Specification

Repo: `meshpigeon-firmware`. Design goal: **the dumbest possible durable radio.**
No protocol, no keys, no message semantics, no repeat logic. It maximizes
retained packets, persists its settings, and streams raw packets to whoever
connects.

## 1. Behavioral spec

### 1.1 Receive & store

- Always listening per persisted settings from power-on (no app needed).
- Every over-the-air packet received (valid CRC) is appended to the **packet
  store** with its `uptime_ms` and radio stats.
- Store is a ring buffer sized to hold the **maximum possible packets**:
  - ESP32-S3: target ≥ 2,000 packets in PSRAM/RAM (≈ 256 B/entry incl.
    metadata → ~512 KB), plus optional flash-backed spill.
  - nRF52840: ≥ 4,000 packets in RAM/flash depending on board.
  - Entry: `[seq:u32][uptime_ms:u32][rssi:i8][snr:i8][len:u8][flags:u8][raw:≤200B]`.
  - The firmware cannot inspect packets (no protocol), so **raw is all it
    keeps** — this is what maximizes capacity.
- Overflow drops the **oldest** entries; `GET_INFO` reports store stats so the
  app can display "history depth".

### 1.2 Send

- `SEND_PACKET` → key the radio once with the raw bytes (≤ 255 B), report
  tx-complete + (optional) CAD result. No retry logic, no ACK logic — the app
  decides retries by listening for on-air ACKs through the same store.

### 1.3 No repeating in the firmware (deliberate)

The firmware **never rebroadcasts on its own**. It cannot know what is safe to
repeat (dedup, loops, freshness) without protocol semantics, and giving it
opinions violates the "all protocol in the app" rule.

- The **repeater function lives in the app**: while connected, the app dedups
  by packet tag and re-sends eligible packets via `SEND_PACKET` (03 §4). The
  radio is a pure forwarder of what the app decides — no identity, no adverts,
  no keys, no status from the radio itself.
- Accepted tradeoff: a radio only repeats while its app is connected. That is
  fine for the product (most folks won't run repeat mode), and standalone
  MeshCore repeaters remain the right tool for always-on infrastructure.
- No `SET_MODE repeat`, no dedup table, no repeat persistence in the firmware —
  the entire concept is absent from the codebase.

### 1.4 Settings persistence

- Radio settings (freq, BW, SF, CR, power, region preset id) stored in
  NVS/flash on every `SET_RADIO`, applied at boot. A radio that reboots alone
  resumes listening with no app present.
- First-boot default: a safe default preset (region 0 / "UNSET" → listens on
  the app's default region; app prompts setup on first connect).

### 1.5 Clock

- Free-running 32-bit `uptime_ms` (wraps ~49.7 days; wrap handled by a boot
  count persisted alongside settings). No RTC required. Time correlation is
  app-side (03 §5).

### 1.6 What the firmware never does

- Never decrypts, parses payload types, tracks contacts, stores keys, sends
  adverts, or talks to the internet. No companion telemetry. No official map.

## 2. Supported boards (v1)

| Board | MCU | Radio | Transport priority |
|---|---|---|---|
| XIAO WIO (ESP32-S3) | ESP32-S3 | SX1262 | USB-CDC + BLE |
| Heltec V3 / T114-class nRF52 | nRF52840 | SX1262 | BLE + USB-CDC |
| (stretch) ESP32 Wi-Fi boards | ESP32 | SX1262 | Wi-Fi TCP + BLE |

## 3. Transport framing (uniform across BLE/USB/Wi-Fi)

- **SLIP-style COBS framing** with 2-byte CRC-16 and a 1-byte command code.
- BLE: Nordic-UART-Service-compatible characteristics (write: host→radio,
  notify: radio→host) so generic tools also work; supports multiple connected
  centrals (see 05 §3).
- USB-CDC: same frames, 115200 or line-coding-native.
- Wi-Fi: raw TCP server (mDNS-advertised `meshpigeon-radio._tcp`), same frames.

## 4. Command set (summary — full spec in `docs/protocol.md` in the repo)

`GET_INFO`, `GET_RADIO`, `SET_RADIO`, `SEND_PACKET`,
`FETCH_PACKETS(since_seq)` (streamed), `PURGE_STORE`, `PING`, `BOOTLOADER`
(reboot to DFU/serial download for in-app flashing), plus async
`RX_PACKET(event)` pushes while connected (in addition to the store).
`GET_INFO` may include an optional battery field (mV/percent from one ADC
read) — **device status**, not mesh telemetry (11 §2.8).

Every command gets a response frame with matching nonce. Errors are numeric +
short string.

## 5. In-app flashing (nice-to-have, phase 2)

- nRF52: standard Nordic DFU over BLE (use existing DFU libs).
- ESP32: ROM serial bootloader over USB-CDC (the app drives the
  esptool protocol; ROM is in-silicon, no pre-flashed bootloader needed).
- The firmware repo publishes signed `.zip`/`.bin` release artifacts the app
  can offer: "Update radio firmware" appears in Advanced.

## 6. Firmware implementation notes

- Language: C++ (Arduino-ESP32 / Arduino-nRF52 or Zephyr later), RadioLib for
  the SX126x.
- Structure: `main/` + `boards/<board>/` ports; **all** logic (ring buffer,
  framing, settings, uptime) in board-neutral code with host-side unit tests
  compiled for desktop (see 09 §3).
- Power: conservative — no deep sleep in v1 (must stay listening/repeating);
  battery gate only for tx duty.

## 7. Acceptance checklist (per board)

- [ ] Boots and listens on persisted settings with no app attached.
- [ ] Survives 1,000-boot settings persistence soak.
- [ ] Stores ≥ target packet count; overflow drops oldest cleanly.
- [ ] `FETCH_PACKETS` replay lets a fresh app reconstruct exact history and times.
- [ ] Repeat behavior verified **app-side only**: a connected MeshPigeon app
      forwards packets correctly; the radio alone never transmits without a
      `SEND_PACKET`.
- [ ] 3 concurrent BLE clients can fetch history simultaneously without
      corrupting the store.
- [ ] Coexists with MeshCore repeaters on-air (interop field test, see 09 §4).
