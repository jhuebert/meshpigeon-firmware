#ifndef MESHPIGEON_PROTO_ALIAS_H
#define MESHPIGEON_PROTO_ALIAS_H

/**
 * Canonical C++ names for the generated nanopb message structs.
 *
 * nanopb flattens package + message into a C identifier
 * (meshpigeon_ClientToRadio); the alias keeps our code and docs naming the
 * contract the way the .proto files do. Also pulls in the generated headers,
 * so this is the one include our sources need for the protobuf types.
 */

#include "meshpigeon/device.pb.h"
#include "meshpigeon/envelope.pb.h"
#include "meshpigeon/radio.pb.h"

namespace meshpigeon {

using ClientToRadioMessage = meshpigeon_ClientToRadio;
using RadioToClientMessage = meshpigeon_RadioToClient;

using DeviceInfoMessage = meshpigeon_DeviceInfo;
using StoreInfoMessage = meshpigeon_StoreInfo;
using DeviceSettingsMessage = meshpigeon_DeviceSettings;
using SetDeviceSettingsMessage = meshpigeon_SetDeviceSettings;
using StatusMessage = meshpigeon_Status;
using AuthMessage = meshpigeon_Auth;

using RadioSettingsMessage = meshpigeon_RadioSettings;
using SetRadioSettingsMessage = meshpigeon_SetRadioSettings;
using PacketEntryMessage = meshpigeon_PacketEntry;
using PacketAcceptedMessage = meshpigeon_PacketAccepted;
using FetchEndMessage = meshpigeon_FetchEnd;
using TxResultMessage = meshpigeon_TxResult;

using ErrorCode = meshpigeon_Error_ErrorCode;

// nanopb spells its per-message constants with the generated name; alias
// them so our code talks about ClientToRadio/RadioToClient throughout.
#define ClientToRadioMessage_init_zero meshpigeon_ClientToRadio_init_zero
#define RadioToClientMessage_init_zero meshpigeon_RadioToClient_init_zero
#define ClientToRadioMessage_fields meshpigeon_ClientToRadio_fields
#define RadioToClientMessage_fields meshpigeon_RadioToClient_fields

// The oneof variant discriminators, by operation.
enum ClientOp : pb_size_t {
    kOpNone = 0,
    kOpPing = meshpigeon_ClientToRadio_ping_tag,
    kOpGetDeviceInfo = meshpigeon_ClientToRadio_get_device_info_tag,
    kOpGetRadioSettings = meshpigeon_ClientToRadio_get_radio_settings_tag,
    kOpSetRadioSettings = meshpigeon_ClientToRadio_set_radio_settings_tag,
    kOpSendPacket = meshpigeon_ClientToRadio_send_packet_tag,
    kOpFetchPackets = meshpigeon_ClientToRadio_fetch_packets_tag,
    kOpPurgeStore = meshpigeon_ClientToRadio_purge_store_tag,
    kOpGetDeviceSettings = meshpigeon_ClientToRadio_get_device_settings_tag,
    kOpSetDeviceSettings = meshpigeon_ClientToRadio_set_device_settings_tag,
    kOpGetStatus = meshpigeon_ClientToRadio_get_status_tag,
    kOpAuth = meshpigeon_ClientToRadio_auth_tag,
    kOpReboot = meshpigeon_ClientToRadio_reboot_tag,
    kOpFactoryReset = meshpigeon_ClientToRadio_factory_reset_tag,
    kOpBootloader = meshpigeon_ClientToRadio_bootloader_tag,
};

}  // namespace meshpigeon

#endif  // MESHPIGEON_PROTO_ALIAS_H
