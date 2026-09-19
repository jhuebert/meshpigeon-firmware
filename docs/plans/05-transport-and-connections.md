# 05 — Transport & Radio Connections

Repo: `meshhop-app`, module `:core-transport`. Goal: connect to any supported
radio over any link, connect to several radios at once, and silently prefer the
radios the user likes — without the user ever changing their identity to do it.

## 1. `RadioAdapter` SPI

```kotlin
interface RadioAdapter {
    val state: StateFlow<RadioLinkState>      // disconnected/scanning/connected/radioReady
    val frames: SharedFlow<RadioFrame>        // decoded command/response frames
    suspend fun send(frame: RadioFrame)
    suspend fun connect(target: RadioTarget): RadioSession
    fun scan(): Flow<RadioTarget>             // BLE scan, USB attach events, mDNS
}
sealed interface RadioTarget {                 // link-agnostic identity for prefs
    val persistentId: String                   // stable across reboots
    val name: String
    val link: RadioLink                       // BLE(mac) | USB(vid/pid/port) | WIFI(ip/port)
}
```

Implementations:

| Adapter | Android APIs | Notes |
|---|---|---|
| `BleAdapter` | `BluetoothLeScanner`, GATT client | Multi-connect capable (§3) |
| `UsbCdcAdapter` | `UsbManager`, CDC-ACM host | Permission prompt per device, remembered |
| `TcpWifiAdapter` | Socket + mDNS (`NsdManager`) | For Wi-Fi boards / desktop testing |

`RadioSession` owns: the framing codec, a command queue with nonces/timeouts,
an RX push stream, and a re-anchor timer for uptime→wall-clock mapping (03 §5).
Everything above this layer (domain) only ever sees decoded packets and
`RadioLinkState`.

## 2. Radio selection: preference order + one-time override

- The user's **radio list** is stored per identity-independent app profile:
  ordered list of `persistentId`s with a flag `preferred`.
- **Auto mode (default):** at connect time the app scans, then connects to the
  first available radio in preference order. Falls back silently. UI shows a
  small radio chip (" Connected: *Pocket Node*") the user can tap to see/change.
- **One-time override:** from the radio chip → "Choose radio…" lists all found
  targets; picking one connects for this session only and does not reorder
  preferences. A "always prefer this one" toggle in that sheet updates the
  order.
- Reconnect policy: exponential backoff scan loop while the foreground service
  runs; if the preferred radio reappears and the user set it as strict
  preference, the app offers (toast) to switch.

## 3. Multi-client radio sharing (BLE)

Requirement: several phones on one radio simultaneously; first-connected user
defines radio settings; others use or override.

- Firmware supports N concurrent BLE centrals (04 §2); each session is
  independent for fetch/send.
- **Settings ownership:** the radio keeps a `config_epoch` counter.
  - Whoever issues `SET_RADIO` bumps the epoch; all clients get an async
    `RADIO_CHANGED(epoch, settings)` event.
  - Clients compare against their **desired settings** (stored locally, per
    app profile). If they differ *and* the radio is not in "first-owner lock"
    (a 5-minute grace period from boot during which only the first `SET_RADIO`
    is honored), the app shows a non-blocking banner:
    *"Radio is set to EU-868. Your saved settings are US-915. [Use radio's] [Apply mine]"*.
  - "Apply mine" re-issues `SET_RADIO` (and bumps epoch); "Use radio's" saves
    the radio's settings as desired for that radio target. Disconnecting keeps
    whatever the user last chose — matches "CR is different" example.
- USB/Wi-Fi adapters: single-client by nature (physically), but the same
  epoch logic applies over Wi-Fi.

## 4. Connection config ≠ radio settings (hard separation)

Two distinct, separately-edited stores:

| Store | Contents | Edited where |
|---|---|---|
| **Radio connection** (per radio target) | link type, name, auto-connect order, notification style | Settings → Radios |
| **Radio settings** (on the radio) | freq/BW/SF/CR/power, region preset, mode | Settings → Radio settings (per connected radio), Advanced-friendly but discoverable |

The app never conflates "which radio I'm talking to" with "how that radio is
tuned." Switching radios never retunes anything unless the user says so.

## 5. Duty to the newcomer

- Onboarding only *scans and lists* radios with friendly names; no pairing
  dialogs unless the OS demands them; no channel/frequency questions until
  region selection (03 §8), which is a two-tap picker.
- If no radio is found, the app continues in offline mode (see 06 §5) with a
  calm banner explaining how to connect later — the user can still explore the
  app fully.

## 6. Reliability details

- Frame CRC + nonce matching; stale nonces dropped; store fetch uses
  resumable `since_seq` cursors so interrupted syncs don't re-pull.
- Uptime re-anchor every 15 min while connected; drift correction is monotonic
  (never rewinds message timestamps).
- BLE: connection-parameters tuning for throughput on packet bursts; GATT
  errors trigger a silent re-connect once before surfacing a problem.
