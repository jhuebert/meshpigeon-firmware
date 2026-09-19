# 09 — Testing Strategy

Both repos get comprehensive testing per the software-development requirements:
layered code that is unit-testable by construction, plus a hardware bench.

## 1. App (`meshpigeon-app`)

| Layer | Tooling | Target |
|---|---|---|
| `:core-protocol` | JUnit + kotlinx.coroutines.test, golden vectors | ≥ 90 % line coverage; round-trip property tests (kotlinx-times? no — junit-quickcheck style via custom generators) |
| `:core-domain` | JUnit, fake repositories, Turbine for flows | ≥ 85 % use-case coverage |
| `:core-transport` | Robolectric for BLE/USB shims + in-memory fake `RadioAdapter` | State machines fully covered |
| `:core-data` | Room in-memory tests, migration tests | Repository contracts |
| `:feature-*` | Compose UI tests (createAndroidComposeRule) + Robolectric | Critical flows: onboarding, send/receive, search, blocking |
| E2E | instrumented tests driving two fake radios + one real radio profile | Smoke suite for CI |

Key suites:

- **Protocol golden vectors:** encode/decode against fixed byte streams
  captured from MeshCore-compatible firmware; every payload type; path-size
  variants 1/2/3 bytes; **hop-count derivation tests** (byte length ≠ hop
  count for multi-byte hashes — a real bug class in meshcore-open, 11 §2.3);
  corrupt-packet handling (CRC fail, truncated, unknown version → drop, never
  crash).
- **ACK/retry state machine:** sim tests over virtual time — send, duplicate
  ACK, ACK-after-retry, expiry → failed.
- **Uptime→wall-clock mapping:** wrap-around, reconnect anchors, drift.
- **ReceivePipeline:** adversarial inputs (malformed adverts, blocked sender
  floods, duplicate tags, out-of-order store fetches).
- **Notification policy matrix:** per-channel modes × conversation kinds.
- **Search:** FTS correctness incl. unicode/emoji.

## 2. Deterministic radio simulation (shared design)

`meshpigeon-firmware/sim/` provides a **desktop radio simulator** implementing
the identical command framing (04 §4) with scriptable on-air behavior
(loss %, latency, dupes). Both repos test against it:

- App CI runs full pipelines against the simulator — no hardware needed.
- Firmware CI runs its host-compiled core against scripted frame sequences.
- A `mesh-sim` harness connects N simulated radios with an RF loss model to
  exercise the app-driven repeater (dedup/rebroadcast), direct-path fallback,
  and app retry logic at mesh scale — **all in CI with zero hardware**.
- The simulator is the primary development loop: app and firmware logic are
  built and verified against it first; hardware is only touched for real-RF
  validation (§3 bench, §4 interop).

## 3. Firmware (`meshpigeon-firmware`)

- **Host-side unit tests** (native, run in CI): ring buffer overflow/order,
  settings persistence (against a flash emulation), SLIP/COBS
  framing, uptime wrap. No dedup/repeat logic exists to test — by design.
- **Board smoke test firmware** (`boards/<board>/smoke`): self-test radio init,
  tx/rx loopback between two boards, store integrity — outputs PASS/FAIL over
  USB; wired into the bench rig below.
- **Hardware bench (in-repo scripts):** two XIAO WIO + one nRF52 board on a
  powered USB hub; `scripts/bench.sh` runs flashing → smoke → app-driven
  scenario via the simulator bridge; results into a SQLite/JSON report.
- Soak tests: 72 h listening with 1 packet/min; store replay equality check.

## 4. Interop (field) testing

- Bench: MeshPigeon ↔ MeshCore companion/repeater exchanges (adverts, GRP_TXT,
  DM, ACK) using the local MeshCore tree and `meshcore_py` tooling already in
  this workspace; documented vectors feed back into §1 golden tests.
- Field checklist: two-node direct, three-node via repeater, app-driven
  repeater rebroadcast under real traffic, multi-phone single-radio sharing
  (05 §3).

## 5. CI gates

- `meshpigeon-app`: PR → lint (ktlint/detekt), unit tests, Robolectric UI tests,
  assemble release; nightly instrumented suite.
- `meshpigeon-firmware`: PR → host tests + build all board targets;
  bench job runs only on `bench-ready` label (hardware connected).
- Coverage ratchets: configured minimums enforced, no silent drops.

## 6. Release testing

- Play internal testing track before each release; firmware releases get a
  tagged bench run + interop spot check before publish.
