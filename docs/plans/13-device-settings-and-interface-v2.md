# 13 — Device Settings, Capabilities & Status (Radio Protocol v2)

*Implemented, September 2026 — the semantics below shipped in
`docs/radio-protocol.md` v2; the wire encoding came from plan 14.*

Repo: `meshpigeon-firmware`, based on **`main`** (the lean single-repo core in
`lib/meshpigeon-core/` + `src/`; no mirror, no alignment). Companion to
[docs/radio-protocol.md](../radio-protocol.md) (the normative wire contract,
currently v1) and plan [04-firmware.md](04-firmware.md). Plan 12 is retired
and is not a dependency of this plan.

> **Superseded in part.** The protobuf interface has been adopted — see
> [14-protobuf-interface-options.md](14-protobuf-interface-options.md). That
> plan replaces this plan's **wire encoding** (§2–§12: TLV tags, the version
> policy, the command table) with protobuf envelopes over the unchanged COBS
> transports. Everything **semantic** here remains normative and carries over
> unchanged: the settings catalog and defaults (§6), atomicity and capability
> rejection (§6, §7), the PIN/auth model (§8), name and advertising behavior
> (§9), Wi-Fi lifecycle, persistence and reset behavior (§10), status fields
> (§11), and the RAM-only packet-store guarantee (§16).

**Goal:** let a companion app configure the *device* — its advertised name, who
may use it (Bluetooth PIN), and its Wi-Fi station — without the firmware
learning anything about any mesh protocol. Along the way, restructure the
info/status side of the interface so it can grow additively forever, and tell
the app what the board can actually do (Wi-Fi? battery?) instead of letting it
guess.

A protobuf-based interface has been adopted — see
[14-protobuf-interface-options.md](14-protobuf-interface-options.md), which
replaces this plan's *wire encoding* (§3–§12). Every semantic decision here —
defaults, atomicity, the auth model, capabilities, status fields, factory
reset, and the RAM-only packet store (§16) — carries over unchanged: those
are encoding-independent.

**Why restructure now:** nothing ships against this interface yet. v1's
fixed-offset blobs (`GET_INFO` = 49 bytes at fixed offsets) are compact but
every addition moves bytes. v2 moves info and device settings to a
**TLV (tag–length–value) encoding** so that every future addition appends a tag
and touches nothing else. This is the last free restructure; the versioning
policy in §2 is written to make v3 unnecessary.

The radio-tuning path (`GET_RADIO` / `SET_RADIO`, 17-byte blob) is **unchanged**
in v2: it is fixed-size by nature, already versioned internally by its own
leading blob-version byte, and works.

---

## 1. What this plan delivers, mapped to the product needs

| Need | Design answer |
|---|---|
| Set device name; name controls the BLE scan/advertising name | `SET_DEVICE_SETTINGS` tag `0x20`; `BleSink` updates the GAP name and restarts advertising (§9) |
| Prepopulate the name with a sensible default | Effective name = stored name, else `MeshPigeon-XXXX` derived from the MAC (§9) — the scheme v1 already advertises; `GET_DEVICE_SETTINGS` returns the effective name so the app's UI prefills |
| Bluetooth PIN to gate use of the node even with no identity on it | Protocol-level `AUTH` command gated by a persisted PIN (§8) — one mechanism uniform across BLE, USB and Wi-Fi TCP. Default is **no PIN** (out of the box the node just works, like every Bluetooth device); when the user opts in, the app suggests a 6-digit code, accepted range 4–8 digits |
| Wi-Fi settings, only on boards that support Wi-Fi | Wi-Fi TLVs in `SET_DEVICE_SETTINGS`, rejected atomically with `ERR_NOT_SUPPORTED` on boards without the capability (§7, §10) |
| Tell the client what the board can do | `capabilities` bitmask TLV in `GET_INFO` (§6), assembled from compile-time board defines |
| Wi-Fi connection status incl. IP address, on any transport | `GET_STATUS` command + `DEVICE_STATUS_CHANGED` async event (§11) |
| Default TCP port | Wi-Fi TCP server defaults to **5000** (the port convention MeshCore's desktop tooling standardized on), overridable via the `wifi_port` setting (§10) |
| Return the interface version in the info fetch | Kept and formalized: byte 0 of `GET_INFO` = interface version, now 2 (§2) |
| Multi-client on every transport | Wi-Fi TCP is **multi-client**, exactly like ESP32 BLE: one `FrameReader` + sink per socket, broadcast to all (§10.2). The sim already proves the model — it serves many TCP clients today |
| Room for future settings/status without protocol churn | Single TLV registry, unknown tags skipped, additive-only policy (§2), brainstorm triage (§13) |

---

## 2. Interface versioning policy

- Byte 0 of the `GET_INFO` response is the **interface version**. v1 firmware
  already put `MESHPIGEON_PROTOCOL_VERSION` there; v2 keeps the position and
  bumps the value. The app refuses to talk to a radio whose major version it
  does not implement, and the firmware never branches on the client's version
  (it only reports its own).
- **From v2 onward, the version byte stays at 2 for all additive change.**
  A TLV every receiver must skip is by definition backwards compatible:
  - *Adding* a TLV tag to a response, or a TLV to a request payload → no bump.
    Older firmware ignores unknown tags; older apps skip unknown tags.
  - *Adding* a new command code → no bump (peers that don't know it reply
    `ERR_BAD_CMD`, which every client already handles).
- A **major bump** (v3) is reserved for: removing or renumbering a tag,
  changing a field's width/semantics, or changing framing itself. The TLV
  registry (§12) is deliberately padded so the plausible additions of the next
  few years fit without any of those.
- Unknown status codes are treated by the app as a generic failure with the
  payload as a hint string — adding status codes is additive too.
- Every version bump after v2 must ship with a one-paragraph justification in
  docs/radio-protocol.md, per that document's normative contract.

**Tag-skipping rule (normative):** a receiver that encounters a TLV tag it
does not know MUST skip `2 + len` bytes and continue parsing. It MUST NOT
reject the payload, and MUST NOT echo unknown tags back.

---

## 3. v2 command surface

| cmd | Name | Direction | Payload |
|---|---|---|---|
| 0x09 | `GET_DEVICE_SETTINGS` | request → response | empty → TLV stream (§7) |
| 0x0A | `SET_DEVICE_SETTINGS` | request → response | TLV stream (subset of fields) → OK + full TLV stream |
| 0x0B | `GET_STATUS` | request → response | empty → TLV stream (§11) |
| 0x0C | `REBOOT` | request → response | empty → OK, then the device reboots |
| 0x0D | `FACTORY_RESET` | request → response | empty → OK, device resets settings *and history*, then reboots (§10.4) |
| 0x0E | `AUTH` | request → response | `[pin: utf-8 digits]` → OK or `ERR_AUTH_REQUIRED` (§8) |

Async frames (nonce 0):

| cmd | Name | Payload |
|---|---|---|
| 0x14 | `DEVICE_STATUS_CHANGED` | `GET_STATUS` TLV stream, broadcast when connection status changes (§11) |
| 0x15 | `DEVICE_SETTINGS_CHANGED` | `GET_DEVICE_SETTINGS` TLV stream, broadcast to all *other* clients when one changes settings — the `RADIO_CHANGED` analogue |

New status codes:

| Code | Meaning |
|---|---|
| 0x07 | `ERR_NOT_SUPPORTED` — field or feature absent on this board (capability-gated) |
| 0x08 | `ERR_AUTH_REQUIRED` — PIN is set and this client has not authenticated (§8) |

Everything else (framing, COBS, CRC, nonce correlation, `PING`, store commands,
radio commands, `BOOTLOADER`) is byte-for-byte as documented in v1.

---

## 4. `GET_INFO` v2

Response payload = one fixed header byte followed by a TLV stream:

```
0    interface version (2)
1..  TLV stream
```

Tags emitted by v2 firmware (order not guaranteed; clients match by tag):

| Tag | Field | Type | Notes |
|---|---|---|---|
| 0x01 | fw version | `[major:1][minor:1][patch:1]` | parsed from the `kFwVersion` string — v1 hardcoded `0,1` here, which was never right |
| 0x02 | board name | utf-8 ≤ 16 | as v1 |
| 0x03 | uptime_ms | u32 | as v1 |
| 0x04 | boot_count | u32 | as v1 |
| 0x05 | store count | u32 | as v1 |
| 0x06 | store capacity | u32 | byte budget, as v1 |
| 0x07 | store dropped | u32 | as v1 |
| 0x08 | oldest seq | u32 | as v1 |
| 0x09 | radio config epoch | u32 | as v1 |
| 0x0A | battery mV | u16 | 0xFFFF = unknown, as v1 |
| 0x0B | capabilities | u32 | bitmask, §6 |
| 0x0C | radio_ok | u8 | 1 = radio init succeeded; a failed-init radio still answers, and the app can now *know* |
| 0x0D | auth_required | u8 | 1 = a PIN is set and this client has not authenticated (§8) |

Payload size lands around 60 bytes — well inside `MESHPIGEON_MAX_FRAME_PAYLOAD`.
All v1 fields survive with identical meaning; the app-side reader becomes
"header byte + TLV walk", which is the shape every future info field will take.

---

## 5. Capabilities

A `u32` bitmask, tag `0x0B`. Bits defined in v2 (all others reserved, zero):

| Bit | Capability | Set when |
|---|---|---|
| 0 | `CAP_WIFI_STA` | board can join a Wi-Fi network as a station (the two ESP32-S3 envs) |
| 1 | `CAP_BATTERY` | board can report battery millivolts (T114, T1000-E today) |
| 2 | `CAP_BLE` | board has the BLE NUS transport |
| 3 | `CAP_USB_CDC` | board has the USB CDC transport |

Capability is a **compile-time fact**, assembled in `src/main.cpp` from the
board defines `platformio.ini` already carries (`MESHPIGEON_HAS_WIFI` added to
the two ESP32 envs; `CAP_BATTERY` follows the existing
`MESHPIGEON_PIN_VBAT_ADC` presence that `BoardHooks::battery_mv` already
branches on). The firmware neither probes hardware at runtime nor maintains a
board table — a new board env costs zero capability code.

Wi-Fi is the only capability that gates a *writable* setting today; the bitmask
exists so that future gated settings (RTC wall-clock, Ethernet, LEDs — §13)
never need another mechanism.

---

## 6. Device settings

`GET_DEVICE_SETTINGS` → TLV stream of the settings below.
`SET_DEVICE_SETTINGS` → any subset; response is OK + the full post-write TLV
stream (so the app confirms what actually stuck).

| Tag | Field | Type / limit | Default | Notes |
|---|---|---|---|---|
| 0x20 | device name | utf-8, 0..20 bytes, empty = default | *effective default* `MeshPigeon-XXXX` (§9) | drives the BLE advertised name; also a friendly label for the app |
| 0x21 | pin | utf-8, 4..8 ASCII digits | `"0000"` (public default; always set) | protocol-level gate, §8 |
| 0x22 | wifi_enabled | u8, 0/1 | 0 | ignored on non-Wi-Fi boards |
| 0x23 | wifi_ssid | utf-8 ≤ 32 | empty | |
| 0x24 | wifi_password | utf-8 ≤ 63 | empty | WPA2/3 passphrase limit |
| 0x25 | wifi_port | u16 | 5000 | TCP server port |

**Semantics of `SET_DEVICE_SETTINGS`:**

- **Atomic.** All TLVs are validated first; one bad field rejects the whole
  request with nothing applied (`ERR_BAD_PAYLOAD`), one capability-gated field
  rejects it with `ERR_NOT_SUPPORTED` (§7). Partial application invites
  "which half saved?" UI bugs — and we do not want a user to believe a setting
  took when the board can never honor it.
- **Validated.** name length; pin length + digit charset; ssid length;
  password length; port ≠ 0. Values outside spec → `ERR_BAD_PAYLOAD`.
- **Unknown tags skipped** (§2) — a newer app can send newer fields to older
  firmware and everything it does understand still applies.
- **Persists immediately** (same rule as `SET_RADIO`, 04 §1.4) and applies
  live: name → re-advertise (§9), pin → gate immediately, Wi-Fi →
  connect/disconnect per `wifi_enabled` (§10).
- **No first-owner lock.** The 5-minute `SET_RADIO` lock protects RF tuning
  from client contention; device identity is a once-per-device decision and
  contention is self-healing (last writer wins, everyone else gets
  `DEVICE_SETTINGS_CHANGED`).
- Broadcasts `DEVICE_SETTINGS_CHANGED` (full TLV stream) to all other
  connected clients after an accepted write — the `RADIO_CHANGED` analogue.
- `GET_DEVICE_SETTINGS` **requires auth** (the Wi-Fi password lives in
  there). The PIN is **write-only on the wire** — no PIN information is ever
  returned; the app remembers what it set in its own database (plan 14
  refinement: the brief `pin_is_default` idea was dropped too).

### 6.1 Why device settings don't get their own fixed blob

The 17-byte `RadioSettings` blob works because every field is fixed-width.
Name, SSID and PIN are strings of variable length; a fixed blob would either
waste 100+ bytes per frame or grow on every addition — exactly what v2 exists
to stop. TLVs make each field independently versionable and skippable.

---

## 7. Capability-gated writes

`SET_DEVICE_SETTINGS` containing any Wi-Fi tag (0x22–0x25) on a board without
`CAP_WIFI_STA` → `ERR_NOT_SUPPORTED`, nothing applied. Same rule generalizes to
any future capability-gated field. The app learns capabilities from `GET_INFO`
and hides the fields itself; the firmware rejects as a backstop so a stale or
buggy client can never leave a user looking at a setting that silently does
nothing.

---

## 8. Access control: the PIN

The node holds no identity and no keys (protocol §8 — "a stolen radio leaks
nothing"). The PIN exists purely to stop *casual* use: a stranger scanning
Bluetooth, or someone on the same LAN discovering the TCP port.

**Chosen mechanism: protocol-level AUTH, not BLE GAP pairing.**

- After connect, a client may always send `PING`, `GET_INFO`, `AUTH` and
  `BOOTLOADER`. Every other command → `ERR_AUTH_REQUIRED` until the client
  authenticates.
- `AUTH [pin]` → `STATUS_OK` marks that sink authenticated (per-connection
  state, not global); wrong pin → `ERR_AUTH_REQUIRED` again. After 3 failed
  attempts the firmware delays 1 s per further attempt (slow brute force,
  cost: one counter).
- `GET_INFO` carries `auth_required` (tag `0x0D`) so an unauthenticated app
  can show a "this node is locked, enter PIN" UI instead of a wall of errors.

**Defaults and format — how every other device behaves:**

- **Always set; the default is the public `"0000"`.** The device ships with
  the classic Bluetooth default PIN — knowable without any documentation —
  so the gate mechanism is uniform and the spec never deals with an
  "absent PIN" state. Out of the box the app simply authenticates with
  `0000`, which is exactly how every Bluetooth device's default PIN behaves.
  A user can replace it in the app; the replacement is what actually
  protects the node.
- **When customized: 4–8 ASCII digits.** Six digits is the modern convention
  users meet everywhere (BLE passkey prompts, phone-to-phone pairing); 4–8
  covers the classic "0000"-style codes and keeps entry fast. No arbitrary
  strings.
- The PIN is per-device, set by the user in the app, persisted on the radio,
  and applied identically on every transport — including Wi-Fi TCP, where no
  pairing concept exists at all.

Rationale, for the record:

1. **Uniform across transports.** BLE pairing protects only BLE; the Wi-Fi
   TCP transport has no pairing concept, and USB CDC has none either. The PIN
   must gate all three or it gates nothing. Protocol-level auth is identical
   everywhere and identical for MeshCore, Meshtastic and any future protocol —
   the firmware still speaks only its own dumb command set.
2. **Host-testable.** The auth gate is pure `CommandProcessor` logic and lives
   in `test/test_core.cpp`. GAP passkey setup is per-stack (NimBLE vs
   Bluefruit), hardware-only, and would triple the hardware test matrix.
3. **Dumb stays dumb.** No bonding tables, no key storage, no crypto in flash —
   the security posture of "a stolen radio leaks nothing" is preserved.
4. **Threat model is honest.** A static PIN over plaintext frames stops casual
   misuse, not a determined attacker with a sniffer. That is the right level
   for a device whose worst-case leak is the ability to send packets the
   *sender* already encrypted.

A future major version could add real pairing/bonding as an *additional*
layer without invalidating this one. BLE GAP passkey is explicitly **not**
planned: it fragments the UX (one pairing flow on BLE, none on Wi-Fi) for no
additional protection of anything the radio can actually lose.

---

## 9. Device name & BLE advertising

- **Effective name rule:** stored name if non-empty, else `MeshPigeon-` +
  4 hex chars derived from the BLE address's low bytes — the same scheme v1
  firmware already advertises (`transports.h` builds it from
  `NimBLEDevice::getAddress()` / the Bluefruit address), so two pigeons in
  range stay distinguishable out of the box and scan lists don't change
  character on upgrade. The default is **derived, never persisted**:
  `GET_DEVICE_SETTINGS` returns the effective name (so the app prefills its
  rename field), but the store holds an empty string until a user actually
  names the bird.
- `SET_DEVICE_SETTINGS` with tag `0x20` updates the GAP name and restarts
  advertising:
  - ESP32/NimBLE: `NimBLEDevice::setDeviceName()` + advertising restart. Live
    connections are unaffected; the new name appears at the next scan.
  - nRF52/Bluefruit: `Bluefruit.setName()` + `Bluefruit.Advertising` restart,
    same semantics.
- Name length ≤ 20 bytes keeps the name inside the standard 31-byte
  advertisement alongside the NUS UUID; no scan-response dependency.
- The name is device-level, not transport-level: it labels the *radio*. The
  app is free to display its own nickname per device locally (app DB), which
  is why the firmware only needs one name.

---

## 10. Wi-Fi station

Scope for v2: **station mode only, one network, DHCP, multi-client TCP.**

### 10.1 Lifecycle

- Persisted with the other device settings; applied at boot when
  `wifi_enabled` is 1 and the board has `CAP_WIFI_STA` — a headless pigeon on
  a shelf comes back on Wi-Fi with no app attached, same contract as radio
  settings (04 §1.4).
- `wifi_enabled` / SSID / password / port are applied live by
  `SET_DEVICE_SETTINGS`: enable → connect; disable → disconnect + close server;
  credential change while enabled → reconnect with new creds.
- Reconnect: exponential backoff 5 s → 2 min cap, forever. A wrong password is
  not fatal — it surfaces as `WIFI_STATE_AUTH_FAIL` in status and the backoff
  keeps retrying (the user may have rotated the password; the app can fix it
  over BLE, which is why status is readable on every transport).
- No AP-mode fallback, no captive portal in v2 — out-of-box setup is BLE-first
  anyway (§13 tracks AP mode as a later capability).
- ESP32-S3 runs Wi-Fi + NimBLE concurrently; the Arduino core's coexistence
  handles arbitration. LoRa TX/RX is untouched — it lives on the radio chip.

### 10.2 TCP transport (multi-client)

- New `WifiTcpSink` in `src/transports.h` (ESP32-only compile), same shape as
  `UsbCdcSink`/`BleSink`: one `FrameReader` + sink registration **per
  connected socket**, `CommandProcessor::broadcast()` delivers async frames to
  all of them. This is exactly the model `BleSink` already uses for multiple
  centrals and the simulator already serves — TCP on real hardware can match
  the same ≥ 3 concurrent clients (`kMaxSinks = 4` registry; RAM cost is one
  ~250-byte reader buffer plus socket state per client, trivial on the S3).
- **Default port 5000** — the port convention MeshCore's TCP tooling
  standardized on, so desktop test tooling written for MeshCore radios feels
  at home. Overridable via `wifi_port` (0x25).
- mDNS: advertise `meshpigeon-XXXX.local` (same MAC suffix as BLE) with the
  NUS-over-TCP service, so a desktop app can discover pigeons on the LAN
  without typing IPs. Same derivation rule as the BLE name (§9) — one suffix,
  used everywhere.
- No TLS, no cloud, no outbound connections. The firmware never initiates a
  network connection; it listens. (A pigeon that phones home is a pigeon with
  opinions.)

### 10.3 Persistence

- **ESP32 (NVS, `Preferences`, namespace `meshpigeon`):** new keys beside the
  existing `radio` / `boots`: `name` (string), `pin` (string), `wifie` (u8),
  `wifissid` (string), `wifipass` (string), `wifiport` (u16).
- **nRF52 (InternalFS):** one new file `/dev.bin` in the same
  remove-then-write pattern as `radio.bin`/`boots.bin` (the BSP's
  `FILE_O_WRITE` appends rather than truncates), holding a small
  length-prefixed record of the same fields.
- The `SettingsStore` seam in `lib/meshpigeon-core` gains
  `save/load(DeviceSettings)`; `MemorySettingsStore` (tests/sim) grows the same
  record. Absent keys/records fall back to defaults on read, so OTA'd
  older→newer firmware upgrades cleanly.
- The Wi-Fi password and PIN live in plaintext on the device — accepted, and
  worth stating plainly: an attacker with flash access has the *device*, not
  the mesh (no identity, no keys live here). Encrypting local config would add
  crypto and key storage to firmware whose entire posture is storing nothing
  worth stealing.

### 10.4 `FACTORY_RESET` and `REBOOT`

- `FACTORY_RESET`: wipe device settings (name, pin, Wi-Fi) back to defaults
  **and clear the packet store** — a user asked for "the pigeon as it shipped",
  and leaving a stranger's packet history behind after a reset would surprise
  anyone who checked. The seq counter restarts. Clearing the store is a
  `clear()` on the RAM ring — there is nothing to erase from flash, because
  packets never live there (§16). `PURGE_STORE` remains the
  history-only wipe for the "keep my settings, clear the airtime log" case.
  Reboots after the wipe. Requires auth when a PIN is set.
- `REBOOT`: clean restart into the persisted configuration. Distinct from
  `BOOTLOADER` (which enters update mode) so the app can offer "restart" for
  troubleshooting without accidentally entering DFU.

---

## 11. Status

`GET_STATUS` → TLV stream. Readable on **every** transport, including over
BLE/USB while Wi-Fi is the active uplink (exactly the "I'm on Bluetooth, what
did the Wi-Fi do?" case) and over Wi-Fi itself, where it costs nothing.

| Tag | Field | Type | Notes |
|---|---|---|---|
| 0x30 | wifi_state | u8 | 0 = off/disabled, 1 = connecting, 2 = connected, 3 = auth failed, 4 = no AP found / error |
| 0x31 | wifi_ssid | utf-8 | currently associated SSID |
| 0x32 | wifi_ip | 4 bytes | station IPv4 address, 0.0.0.0 when not connected |
| 0x33 | wifi_port | u16 | port the TCP server listens on |
| 0x34 | wifi_rssi | i8 | AP signal, 0 = unknown |
| 0x35 | ble_clients | u8 | connected BLE centrals |

- `DEVICE_STATUS_CHANGED` (0x14) broadcasts the full `GET_STATUS` TLV stream
  whenever `wifi_state` transitions. That is the only trigger in v2 —
  per-second RSSI churn would spam BLE notify queues for no app-visible gain;
  the app refreshes RSSI by polling if it wants it live.
- The payload is deliberately identical to `GET_STATUS`'s response so both
  parse with one function.

---

## 12. TLV tag registry

One global registry, grouped by purpose, documented normatively in
docs/radio-protocol.md v2. v2 allocates:

```
0x01–0x0F   GET_INFO fields
0x20–0x2F   device settings
0x30–0x3F   status
```

Reserved for future growth (named now so future plans don't renumber):

```
0x40–0x4F   power / environment status (charge state, temperature, ...)
0x50–0x5F   capability-gated settings (wall clock, Ethernet, LEDs, ...)
0x60–0x6F   diagnostics (reset reason, crash counters, FS stats)
```

Registry rules: tags are never reused with a different meaning; a tag's value
may grow *only* in ways a shorter-read tolerates (fixing widths at allocation
time avoids this entirely); deprecation = stop emitting, never redefinition.

---

## 13. Brainstorm: the rest of the settings/status/info surface

Everything below was considered for v2. Triaged against the guiding
principles (dumb firmware, additive interface, lean code).

### In v2 (covered above)
capabilities · interface version · fw version triple · radio_ok ·
auth_required · device name · PIN/auth · Wi-Fi ssid/pass/enable/port ·
Wi-Fi status (state/ssid/IP/port/RSSI) · BLE client count ·
`DEVICE_STATUS_CHANGED` · `DEVICE_SETTINGS_CHANGED` · `REBOOT` ·
`FACTORY_RESET`.

### Later — tracked, capability-gated, none block v2

| Candidate | Tag range | Why later |
|---|---|---|
| Wall clock set/read (`SET_TIME`) | 0x50 | only meaningful on RTC boards; needs a `CAP_RTC` bit and per-board probe. The uptime-anchor design (protocol §6) deliberately needs no clock; this is a convenience for RTC-equipped boards only |
| Ethernet | 0x50 + `CAP_ETH` | same shape as Wi-Fi when a board with a PHY earns its keep |
| LED / indicator control | 0x50 | cosmetic, board-specific; no user demand yet |
| BLE advertising on/off + interval | 0x50 | power saving for battery pigeons; interacts with stack timings — measure before designing |
| USB CDC on/off | 0x50 | plausible hardening, near-zero demand |
| Multiple Wi-Fi networks / AP-mode fallback | 0x50 | setup robustness; v2's BLE-first setup flow covers the out-of-box case |
| Static IP config | 0x50 | DHCP + mDNS covers it; static IP is enthusiast-only |
| Store retention policy (age-based eviction) | 0x50 | the ring buffer already drops oldest on overflow; an age policy adds a persisted opinion for marginal gain |
| Charge state, temperature, fuel gauge | 0x40 | only where the board exposes it; re-add per capability if an app feature needs them |
| Reset reason / crash counters / FS stats | 0x60 | diagnostics for support; cheap, but no support channel exists yet to use them |
| Hardware revision / serial number | 0x01–0x0F | board name covers identification; a serial implies manufacturing processes that don't exist |

### Rejected — would give the firmware opinions

- **Anything mesh-shaped**: channel/key storage, node IDs, adverts, contact
  lists, repeat schedules, dedup, ACK policy — all app domain
  (GUIDING-PRINCIPLES.md; the whole point is MeshCore/Meshtastic/future-
  protocol neutrality — none of these concepts exist at the device layer).
- **Mesh telemetry fields** in status: the radio cannot parse payloads it
  stores; device-level status only, as v1 §8 already states.
- **Encrypted config store**: §10.3's threat model — nothing on the device is
  worth a key-wrapping scheme.
- **Flash-backed packet storage** ("spill to flash when RAM is full"): never,
  for privacy (no history recoverable from a stolen or confiscated device) and
  flash wear — see §16. This deliberately closes the door that plan
  04-firmware §1.1 left open with its "optional flash-backed spill".
- **Per-protocol profile switching** ("MeshCore mode"/"Meshtastic mode"): the
  firmware has no protocol to switch. The app owns all protocol behavior; the
  device interface is already protocol-neutral by construction.

---

## 14. Implementation plan

Phased so each lands green in CI and bench before the next starts. All work in
`lib/meshpigeon-core/`, `src/`, `test/`, and docs — the core stays
host-testable and board code stays wiring-only.

### Phase 1 — interface v2 core (no new hardware code, fully host-testable)

1. `lib/meshpigeon-core/include/meshpigeon/protocol.h`: version 2, new
   CMD/STATUS/async defines, TLV tag registry constants.
2. New `lib/meshpigeon-core` `tlv.{h,cpp}` (+ host tests): writer builder and
   skipping reader — ~100 lines, the only new mechanism in the whole plan.
3. `command_processor.{h,cpp}`: restructure `GET_INFO` (§4); add
   `GET/SET_DEVICE_SETTINGS`, `AUTH`, `REBOOT`, `FACTORY_RESET`, `GET_STATUS`;
   per-sink `authenticated` state (a plain member on `IFrameSink`, so the sink
   registry needs no new bookkeeping) and the auth gate; failure-attempt rate
   limit. `IBoardHooks` growth: `set_device_name()`, `reboot()`,
   `factory_reset()` hooks, capability bitmask accessor.
4. `settings.h`: new `DeviceSettings` struct (name/pin/wifi fields) with its
   own validation; `SettingsStore` gains `save/load(DeviceSettings)`;
   `MemorySettingsStore` grows the record.
5. `src/main.cpp`: `BoardSettingsStore` gains the NVS keys (ESP32) and the
   `/dev.bin` record (nRF52), §10.3; capabilities bitmask assembled from board
   defines; `BoardHooks` implements name/reboot/factory-reset (NVS `clear()` /
   InternalFS removes).
6. `src/transports.h`: `BleSink::set_name()` (NimBLE + Bluefruit paths, §9).
7. `src/sim_main.cpp`: sim reports its capabilities (TCP-only: no BLE/USB/Wi-Fi
   bits), accepts device settings, honors the auth gate; nothing else changes —
   the sim stays a perfect protocol peer for app CI.
8. Tests (`test/test_core.cpp`): TLV round-trip/skip; `GET_INFO` v2 shape +
   fw-version parse; settings atomicity (bad-tag/bad-value/capability-gate →
   nothing applied); auth gate (deny pre-auth, allow after, rate limit,
   `GET_DEVICE_SETTINGS` locked); `DEVICE_SETTINGS_CHANGED`
   broadcast-except-sender; factory-reset → defaults + empty store.
9. Docs: rewrite docs/radio-protocol.md as normative v2 (this plan is the
   design record; the protocol doc stays the contract).

### Phase 2 — Wi-Fi (the two ESP32-S3 boards)

1. `platformio.ini`: `-D MESHPIGEON_HAS_WIFI` on `xiao_wio` and `heltec_v3`.
2. `src/transports.h`: `WifiTcpSink` (§10.2) — STA lifecycle with backoff,
   multi-client accept loop, `WiFiServer` on the configured port, ESPmDNS
   advertisement; wired into `src/main.cpp` behind the capability define.
3. `GET_STATUS` / `DEVICE_STATUS_CHANGED` fed from the link; command processor
   remains transport-agnostic.
4. Bench: connect via BLE while Wi-Fi up (coexistence), reboot-on-shelf
   resumes Wi-Fi, wrong-password → status 3, ≥ 3 concurrent TCP clients, name
   re-advertises after `SET_DEVICE_SETTINGS`.

### Phase 3 — polish

1. Sim `--fake-wifi` flag: emulates Wi-Fi state transitions for app dev/CI
   without hardware (the sim already speaks real TCP, so this is a state
   machine behind `GET_STATUS`, not new transport code).
2. App-contract notes: hand the :core-transport implementer the v2 doc + the
   tag registry; the acceptance checklist in README.md gains the v2 rows
   (name re-advertises, PIN gates all transports, Wi-Fi resumes after reboot).

### Explicitly out of scope

AP mode, TLS, bonding/pairing, any mesh concept, any new board env.

---

## 15. Resolved decisions

1. **Default TCP port: 5000** — the convention MeshCore's TCP tooling
   standardized on; configurable via `wifi_port`.
2. **PIN: 4–8 ASCII digits; default `"0000"`, public, always set** — the
   gate exists uniformly and the out-of-the-box flow needs no documentation
   (like every Bluetooth device's default PIN); a user-chosen replacement is
   what actually protects the node.
3. **`FACTORY_RESET` clears settings *and* packet history** — "as it shipped"
   is what a user expects a reset to mean; `PURGE_STORE` covers the
   settings-keeping case.
4. **Capability-gated fields: rejected atomically** (`ERR_NOT_SUPPORTED`,
   nothing applied) — the board would never honor them, and a silent no-op
   setting is worse than an error.
5. **TCP is multi-client, like BLE** — one `FrameReader` + sink per socket,
   same `kMaxSinks` registry, same ≥ 3 concurrent-client target. The sim
   already serves multiple TCP clients; hardware TCP costs only a per-socket
   reader buffer.
6. **Packet store: RAM only, always** (§16) — no packet bytes ever written to
   flash, for privacy and flash-wear reasons, superseding plan 04's
   "optional flash-backed spill".

---

## 16. Packet store: RAM only, always

A device-level guarantee, normative for every current and future feature:

- **No packet bytes are ever written to flash.** Not at boot, not on overflow,
  not on purge, not for diagnostics, not under any future "capacity" or
  "persistence" feature flag. The packet store is the malloc'd RAM ring
  buffer it already is on `main`.
- **Why privacy:** the firmware cannot decrypt what it stores (no keys, no
  protocol — that is the design), but ciphertext history is still traffic
  analysis material: who transmitted, when, how often, how large. A radio
  seized or stolen must have no history to recover. Volatile memory makes
  "the past stops at power-off" a hardware property instead of a policy.
- **Why wear:** a busy mesh can append many packets per second; flash has
  finite erase cycles. Persisting the store would burn flash for data the
  design says the device should not keep.
- **What this means operationally:** power loss or reboot clears history, and
  that is *correct behavior*, not a bug. The interface already handles it —
  uptime is a 64-bit monotonic counter (plan 14 §6), so clients re-anchor
  trivially, and a reconnecting app refetches with a fresh `oldest_seq`
  baseline. The store stats in `GET_INFO`
  (`count`, `dropped`, `oldest_seq`) describe the RAM ring only.
- **Complete persistence inventory** — everything the firmware may ever write
  to flash, now and in future plans:
  1. radio settings (region, freq, BW, SF, CR, power, epoch),
  2. boot count,
  3. device settings (name, PIN, Wi-Fi).

  That is the whole list. A code review that finds a fourth writer is looking
  at a bug.

This guarantee is encoding-independent: it holds under the TLV interface in
this plan and equally under a protobuf interface (plan 14), and it must be
restated normatively in docs/radio-protocol.md when v2 lands.
