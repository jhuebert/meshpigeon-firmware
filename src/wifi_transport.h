#ifndef MESHPIGEON_WIFI_TRANSPORT_H
#define MESHPIGEON_WIFI_TRANSPORT_H

#if defined(MESHPIGEON_ESP32) && defined(MESHPIGEON_HAS_WIFI)

#include <Arduino.h>
#include <WiFi.h>

#include "meshpigeon/command_processor.h"
#include "meshpigeon/framing.h"

namespace meshpigeon {

/**
 * Wi-Fi station + multi-client TCP transport (docs/radio-protocol.md §7; ESP32 only — the
 * two ESP32-S3 envs define MESHPIGEON_HAS_WIFI).
 *
 * Lifecycle (§10.1): station mode only, one network, DHCP, applied from the
 * device settings. Enable -> connect; disable -> disconnect + close the
 * server; credential change while enabled -> reconnect. Retries run on an
 * exponential backoff (5 s doubling to a 2 min cap, forever): a wrong
 * password is not fatal — it surfaces as WIFI_STATE_AUTH_FAIL in Status and
 * the user may have rotated the password.
 *
 * Multi-client TCP (§10.2): one FrameReader + sink per connected socket,
 * exactly like the simulator's model — CommandProcessor::broadcast delivers
 * async frames to all of them. Default port 5000 (the MeshCore TCP tooling
 * convention), overridable by the wifi_port setting. mDNS advertises
 * meshpigeon-<suffix>.local with the meshpigeon TCP service so desktop
 * tooling can discover pigeons without typing IPs.
 *
 * No TLS, no cloud, no outbound connections: the firmware listens, it never
 * dials (§10.2).
 */
class WifiTransport {
 public:
  /** `hostname` is the mDNS name ("meshpigeon-A3F2.local"), built from the
   *  same MAC suffix every other derived identifier uses (§9). */
  void begin(CommandProcessor& proc, const char* hostname);

  /** Board loop: service the state machine, accept and drain sockets. */
  void pump();

  /** Device settings changed: apply enable/ssid/password/port live. */
  void apply(const DeviceSettings& settings);

  /** IBoardHooks::fill_status — link state for the Status message. */
  void fill_status(StatusMessage* status);

  size_t client_count() const { return num_clients_; }

 private:
  enum class State {
    OFF,        // disabled or unconfigured
    CONNECTING, // associating, or waiting out the backoff
    CONNECTED,
    AUTH_FAIL,  // the AP rejected us (wrong password?)
    ERROR,      // no such network
  };

  class Client : public IFrameSink {
   public:
    void send_frame(const uint8_t* decoded, size_t len) override;
    WiFiClient client;
    FrameReader reader;
    uint8_t frame[FRAME_MAX_DECODED];
    bool in_use = false;
  };

  void connect_start();
  void disconnect();
  void start_server();
  void stop_server();
  void set_state(State s);
  void accept_clients();
  void pump_clients();
  void drop_client(size_t index);

  CommandProcessor* proc_ = nullptr;
  DeviceSettings settings_;
  State state_ = State::OFF;
  char hostname_[24] = {0};
  WiFiServer* server_ = nullptr;  // owned; the port can change at runtime
  bool server_up_ = false;
  uint16_t port_ = MESHPIGEON_WIFI_PORT_DEFAULT;
  uint32_t last_attempt_ms_ = 0;
  uint32_t backoff_ms_ = 0;

  static const size_t kMaxClients = 4;  // the sink registry's size
  Client clients_[kMaxClients];
  size_t num_clients_ = 0;
};

}  // namespace meshpigeon

#endif  // MESHPIGEON_ESP32 && MESHPIGEON_HAS_WIFI
#endif  // MESHPIGEON_WIFI_TRANSPORT_H
