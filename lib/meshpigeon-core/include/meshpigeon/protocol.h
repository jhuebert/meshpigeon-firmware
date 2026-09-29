#ifndef MESHPIGEON_PROTOCOL_H
#define MESHPIGEON_PROTOCOL_H

/**
 * MeshPigeon radio-interface constants — the public contract documented in
 * docs/radio-protocol.md and specified by the .proto files in
 * protobufs/meshpigeon/. Keep in sync with meshpigeon-app's :core-transport.
 *
 * The firmware speaks no mesh protocol. The interface is how a companion app
 * drives a dumb radio: fetch packets, send packets, set radio, survive
 * reboots, remember history — and since v2, name the device, gate it behind
 * a PIN, and manage its Wi-Fi.
 *
 * v2 re-encodes the frame payload as a serialized protobuf envelope
 * (ClientToRadio / RadioToClient, oneof body). The old [cmd][nonce][status]
 * frame header is gone: correlation (id), operation (oneof variant) and
 * status (Error.code) all live inside the envelope, which is what makes the
 * .proto files the complete wire contract.
 */

#define MESHPIGEON_SPEC_VERSION 2

// ---- Limits --------------------------------------------------------------
// The Semtech SX12xx silicon cap: everything MeshCore and Meshtastic can put
// on the air (docs/radio-protocol.md §2).
#define MESHPIGEON_MAX_RAW_PACKET 255
// Biggest serialized envelope we accept in a frame (docs/radio-protocol.md §2).
// 512 is not "a round number with headroom" — it is the exact size of the
// largest legal envelope: a Pong echoing a maximum-size (500 B) Ping with a
// 5-byte varint id encodes to exactly 512 bytes. Response encoding uses this
// same cap, so a change that grows an envelope (a new field, a wider oneof
// tag) can push the worst case over it and make encode_response() return 0 —
// which drops the answer silently. test_max_size_pong_still_fits_the_frame
// pins the boundary; bump this constant in the same commit that breaks it.
#define MESHPIGEON_MAX_FRAME_PAYLOAD 512
// TX power ceiling in dBm, 0..MESHPIGEON_TX_POWER_MAX (docs/radio-protocol.md §6).
// Both radio ports hand the value to RadioLib as an int8_t, so a power above
// 127 would not merely be out of range — it would wrap NEGATIVE (200 -> -56
// dBm) and the board would key up quietly at the wrong power. The wire field
// is a uint32, so the core must reject the range rather than trust the caller.
#define MESHPIGEON_TX_POWER_MAX 22
// Device-settings limits (mirrored by the nanopb .options caps).
#define MESHPIGEON_NAME_MAX 20
#define MESHPIGEON_PIN_MIN 4
#define MESHPIGEON_PIN_MAX 8
#define MESHPIGEON_SSID_MAX 32
#define MESHPIGEON_PASS_MAX 63
#define MESHPIGEON_WIFI_PORT_DEFAULT 5000

#endif  // MESHPIGEON_PROTOCOL_H
