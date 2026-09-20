#ifndef MESHPIGEON_PROTOCOL_H
#define MESHPIGEON_PROTOCOL_H

/**
 * MeshPigeon radio-protocol constants — the public contract documented in
 * docs/radio-protocol.md. Keep in sync with meshpigeon-app's :core-transport.
 *
 * The firmware speaks no mesh protocol. These commands are how a companion
 * app drives a dumb radio: fetch packets, send packets, set radio, survive
 * reboots, remember history.
 */

#define MESHPIGEON_PROTOCOL_VERSION  1

// ---- Command codes -------------------------------------------------------
// Host -> radio commands (request/response).
#define CMD_PING            0x01
#define CMD_GET_INFO        0x02
#define CMD_GET_RADIO       0x03
#define CMD_SET_RADIO       0x04
#define CMD_SEND_PACKET     0x05
#define CMD_FETCH_PACKETS   0x06
#define CMD_PURGE_STORE     0x07
#define CMD_BOOTLOADER      0x08

// Radio -> host async frames (no request; nonce unused, set 0).
#define CMD_RX_PACKET       0x10   // one packet received/stored on-air
#define CMD_FETCH_END       0x11   // end of a FETCH_PACKETS stream
#define CMD_TX_RESULT       0x12   // per-store-id tx result for SEND_PACKET
#define CMD_RADIO_CHANGED   0x13   // settings changed by another client

// ---- Response status codes ----------------------------------------------
#define STATUS_OK           0x00
#define STATUS_ERR_BAD_CMD      0x01
#define STATUS_ERR_BAD_PAYLOAD  0x02
#define STATUS_ERR_BAD_CRC      0x03
#define STATUS_ERR_BUSY         0x04
#define STATUS_ERR_TX_FAILED    0x05
#define STATUS_ERR_NO_RADIO     0x06

// ---- Limits (from the plan, 04-firmware §1) ------------------------------
#define MESHPIGEON_MAX_RAW_PACKET    200   // raw OTA bytes we keep per entry
#define MESHPIGEON_MAX_FRAME_PAYLOAD 220   // biggest payload we'll accept in a frame

#endif  // MESHPIGEON_PROTOCOL_H
