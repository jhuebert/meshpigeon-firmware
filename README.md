# MeshPigeon Radio Firmware

The dumbest possible durable radio: it receives LoRa packets into the largest
memory it can hold, stamps each with uptime, keys up when told, and persists
its radio settings across reboots. **No mesh protocol, no keys, no repeat
logic** — all of that lives in the [MeshPigeon app](https://github.com/jhuebert/meshpigeon-app).

```
┌──────────────┐   BLE / USB CDC / TCP (raw frames)
│  MeshPigeon App │ ◄──────────────────────────────────┐
│  (all proto) │                                    │
└──────────────┘                                    ▼
                                      ┌──────────────────────────┐
                                      │ MeshPigeon Radio Firmware   │
                                      │ radio → packet store     │
                                      │ (max packets, uptime)    │
                                      │ + persisted radio settings│
                                      └──────────────────────────┘
                                                   ▼
                                        on-air (MeshCore-compatible
                                        flood/direct routing)
```

## Supported boards (v1)

| Board | MCU | Radio | Transports |
|---|---|---|---|
| Seeed XIAO ESP32-S3 + Wio-SX1262 | ESP32-S3 | SX1262 | USB CDC + BLE |
| Heltec WiFi LoRa 32 V3 | ESP32-S3 | SX1262 | USB CDC + BLE |
| Seeed SenseCAP T114 | nRF52840 | SX1262 | USB CDC (BLE lands with first bench bring-up) |

Build a board:

```sh
pio run -e xiao_wio        # or heltec_v3, t114
pio run -e xiao_wio -t upload
pio run -e xiao_wio -t monitor
```

Run the host-side unit tests (no hardware needed — the entire board-neutral
core is compiled and tested on the desktop):

```sh
pio test -e native         # 30 tests: framing, store, settings, commands
```

Run the **desktop radio simulator** — a TCP stand-in for a real board that
speaks the identical protocol, for app development and CI with zero hardware:

```sh
pio run -e sim
.pio/build/sim/program --port 8765 --traffic-ms 3000 --loss 10
```

## Layout

```
lib/meshpigeon-core/   board-neutral core: framing (COBS+CRC16), packet store,
                    settings persistence, uptime clock, command processor
src/main.cpp        board main (wires radio + transports + core loop)
src/radio_sx1262.h  RadioLib SX1262 port (raw bytes only)
src/transports.h    USB CDC + BLE (Nordic UART Service) frame sinks
sim/                desktop simulator (TCP bridge + scriptable RF loss/dup)
test/               host-side unit tests (Unity, run with -e native)
boards/             custom board definitions (seeed_t114)
docs/               radio-protocol.md — the versioned command contract
docs/plans/         founding plan documents
```

## What the firmware never does

- Never decrypts, parses payload types, tracks contacts, stores keys, sends
  adverts, or talks to the internet. It never rebroadcasts on its own —
  if a change gives the firmware opinions, it belongs in the app
  (see [GUIDING-PRINCIPLES.md](GUIDING-PRINCIPLES.md)).

## Acceptance checklist (per board, bench)

- [ ] Boots and listens on persisted settings with no app attached.
- [ ] Survives a settings-persistence soak across reboots.
- [ ] Stores ≥ target packet count; overflow drops oldest cleanly.
- [ ] `FETCH_PACKETS` replay lets a fresh app reconstruct exact history.
- [ ] A connected app forwards packets; the radio alone never transmits
      without a `SEND_PACKET`.
- [ ] 3 concurrent BLE clients can fetch history simultaneously.
- [ ] Coexists with MeshCore repeaters on-air.

## License

MIT — see [LICENSE](LICENSE).
