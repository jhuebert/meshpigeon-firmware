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
// Biggest serialized envelope we accept in a frame. Sized so a worst-case
// PacketEntry (255 raw bytes + metadata) plus envelope overhead fits with
// headroom (docs/radio-protocol.md §2). The generated *_size macros top out at 512.
#define MESHPIGEON_MAX_FRAME_PAYLOAD 512
// Device-settings limits (mirrored by the nanopb .options caps).
#define MESHPIGEON_NAME_MAX 20
#define MESHPIGEON_PIN_MIN 4
#define MESHPIGEON_PIN_MAX 8
#define MESHPIGEON_SSID_MAX 32
#define MESHPIGEON_PASS_MAX 63
#define MESHPIGEON_WIFI_PORT_DEFAULT 5000

#endif  // MESHPIGEON_PROTOCOL_H
