#include "wifi_transport.h"

#if defined(MESHPIGEON_ESP32) && defined(MESHPIGEON_HAS_WIFI)

#include <ESPmDNS.h>

#include <stdio.h>
#include <string.h>

namespace meshpigeon {

static const uint32_t kBackoffStartMs = 5000;
static const uint32_t kBackoffMaxMs = 120000;

void WifiTransport::Client::send_frame(const uint8_t* decoded, size_t len) {
  uint8_t wire[FRAME_MAX_WIRE];
  size_t n = frame_encode_envelope(wire, decoded, len);
  if (n == 0 || client.write(wire, n) != n) {
    // The socket is gone or its buffer is full. Flag it: a dropped frame
    // mid-stream (say, half a FetchPackets burst) would otherwise leave the
    // client believing it received the whole thing, terminated by FetchEnd.
    // The board loop closes the socket on the next pump, so the client sees
    // a disconnect and resumes from its cursor.
    broken = true;
  }
}

void WifiTransport::begin(CommandProcessor& proc, const char* hostname) {
  proc_ = &proc;
  settings_ = DeviceSettings::defaults();
  strncpy(hostname_, hostname, sizeof(hostname_) - 1);
  hostname_[sizeof(hostname_) - 1] = 0;
  stop_server();
  WiFi.mode(WIFI_OFF);
  set_state(State::OFF);
}

void WifiTransport::apply(const DeviceSettings& settings) {
  // Compare against what is in force *before* overwriting it, so no
  // before/after copies of the credentials are needed.
  const bool was_enabled = settings_.wifi_enabled;
  const bool credentials_changed =
      strcmp(settings_.wifi_ssid, settings.wifi_ssid) != 0 ||
      strcmp(settings_.wifi_password, settings.wifi_password) != 0;
  const uint16_t new_port = settings.wifi_port == 0
                                ? MESHPIGEON_WIFI_PORT_DEFAULT
                                : settings.wifi_port;
  const bool port_changed = new_port != port_;

  settings_ = settings;
  settings_.wifi_port = new_port;
  port_ = new_port;  // Status reports the configured port, up or down

  if (!usable()) {
    // Disabled, or enabled with nothing to associate with: the link cannot
    // serve, so tear down one that is still up rather than leaving the node
    // quietly associated with a network it can no longer reach.
    if (was_enabled) {
      stop_server();  // drops the sockets too
      disconnect();
    }
    return;
  }
  if (!was_enabled || credentials_changed) {
    // New network, new credentials, or newly enabled: rebind from scratch.
    stop_server();
    disconnect();
    connect_start();
    return;
  }
  if (port_changed && server_up_) start_server();  // rebind the listener
}

void WifiTransport::connect_start() {
  // Unconditional: a credential change while CONNECTED (or while backing off
  // from AUTH_FAIL/ERROR) is still a transition, and the documented Status
  // push fires on transitions. set_state() is a no-op when the state already
  // matches, so this cannot cause push churn.
  set_state(State::CONNECTING);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // the radio is the point; don't nap the link
  // The Arduino core's own auto-reconnect fires on a timer for reasons it
  // calls reconnectable (NO_AP_FOUND, ASSOC_FAIL, HANDSHAKE_TIMEOUT, ...),
  // which would reconnect immediately and make the ladder below irrelevant —
  // an AP outage would become a tight reconnect loop instead of the
  // documented 5 s -> 2 min backoff. This transport is the only retry
  // authority (docs/radio-protocol.md §7).
  WiFi.setAutoReconnect(false);
  WiFi.begin(settings_.wifi_ssid, settings_.wifi_password);
  last_attempt_ms_ = millis();
  // A fresh attempt ladder: 5 s, doubling to the 2 min cap. Seeding this
  // is what makes the doubling in pump() actually grow — a zero seed
  // doubles to zero and retries on every board-loop tick.
  backoff_ms_ = kBackoffStartMs;
}

void WifiTransport::disconnect() {
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  set_state(State::OFF);
}

void WifiTransport::start_server() {
  stop_server();
  server_ = new WiFiServer(port_);
  server_->begin();
  server_up_ = true;
  // mDNS: meshpigeon-A3F2.local, so desktop tooling finds the pigeon
  // without typing an IP (docs/radio-protocol.md §7).
  if (!MDNS.begin(hostname_)) return;
  MDNS.addService("meshpigeon", "tcp", port_);
}

void WifiTransport::stop_server() {
  // Walk the slots, not the count. drop_client() no-ops on a free slot, and
  // free slots exist: a client that disconnects leaves a hole behind, so
  // "index num_clients_ - 1" is not the last live client and looping on it
  // would spin forever on a hole — a board hang on the next settings
  // change that tore the server down.
  for (size_t i = 0; i < kMaxClients; i++) drop_client(i);
  if (server_ != nullptr) {
    server_->stop();
    delete server_;
    server_ = nullptr;
  }
  server_up_ = false;
  MDNS.end();
}

void WifiTransport::set_state(State s) {
  if (state_ == s) return;
  state_ = s;
  if (proc_) proc_->on_wifi_state_changed();  // only transitions push
}

void WifiTransport::accept_clients() {
  if (!server_up_ || !server_->available()) return;
  WiFiClient c = server_->accept();
  if (!c) return;
  for (size_t i = 0; i < kMaxClients; i++) {
    if (clients_[i].in_use) continue;
    if (proc_ == NULL || proc_->sink_free() == 0) {
      // The sink registry is shared by every transport and is full: turn
      // this one away rather than accept a connection nothing will ever be
      // sent to. Stopping it matters as much as refusing it — an accepted
      // WiFiServer client that is never closed keeps a slot of its own, and
      // stays a connected-but-silent peer on the radio's port.
      c.stop();
      return;
    }
    clients_[i].client.stop();
    clients_[i].client = c;
    clients_[i].reader.reset();
    clients_[i].in_use = true;
    clients_[i].broken = false;
    clients_[i].authenticated = false;
    num_clients_++;
    if (!proc_->add_sink(&clients_[i])) {
      // Lost the race for the last slot: turn the client away rather than
      // accepting a connection nothing will ever be sent to.
      clients_[i].client.stop();
      clients_[i].in_use = false;
      num_clients_--;
      return;
    }
    return;
  }
  // At capacity: the extra client is dropped rather than queued.
}

void WifiTransport::pump_clients() {
  // The per-socket drain is bounded per tick
  // (FRAME_MAX_DRAIN_BYTES_PER_PUMP): a socket that keeps itself fed must
  // not be able to hold the board loop, and `Ping` is auth-exempt, so an
  // unauthenticated peer can be answered indefinitely. The budget is per
  // socket, so one flooder cannot starve the others.
  for (size_t i = 0; i < kMaxClients; i++) {
    Client& c = clients_[i];
    if (!c.in_use) continue;
    if (!c.broken) {
      drain_frames(*proc_, c.reader, c.frame, &c, c.client);
    }
    if (c.broken || !c.client.connected()) {
      drop_client(i);
    }
  }
}

void WifiTransport::drop_client(size_t index) {
  Client& c = clients_[index];
  if (!c.in_use) return;
  if (proc_) proc_->remove_sink(&c);
  c.client.stop();
  c.in_use = false;
  num_clients_--;
}

void WifiTransport::pump() {
  // A disabled or unconfigured link closes everything it opened; apply() has
  // already done that, so there is nothing left to drive (docs §7).
  if (!usable()) return;
  // Exactly ONE status() read per tick. WiFiSTAClass::status() is
  // xEventGroupClearBits(), so reading it consumes the one-shot disconnect
  // reason; a second read in the same tick would swallow the very event this
  // state machine exists to classify (a wrong password's WL_CONNECT_FAILED
  // would be discarded and the app would see CONNECTING for a cycle).
  const wl_status_t st = WiFi.status();
  if (state_ == State::CONNECTED && st != WL_CONNECTED) {
    stop_server();
    set_state(State::CONNECTING);
    // The link dropped: start the attempt ladder over, from the documented
    // 5 s floor, so the retry loop below backs off instead of spinning.
    last_attempt_ms_ = millis();
    backoff_ms_ = kBackoffStartMs;
  } else if (state_ == State::CONNECTING || state_ == State::AUTH_FAIL ||
             state_ == State::ERROR) {
    if (st == WL_CONNECTED) {
      if (!server_up_) start_server();
      backoff_ms_ = 0;
      set_state(State::CONNECTED);
    } else {
      if (st == WL_NO_SSID_AVAIL) {
        set_state(State::ERROR);
      } else if (st == WL_CONNECT_FAILED) {
        set_state(State::AUTH_FAIL);  // most likely a rejected password
      } else {
        set_state(State::CONNECTING);
      }
      if (millis() - last_attempt_ms_ >= backoff_ms_) {
        last_attempt_ms_ = millis();
        // Exponential backoff: 5 s doubling to a 2 min cap, forever. The
        // zero case can only be reached by a path that forgot to seed it;
        // floor it rather than doubling zero into an immediate retry storm.
        if (backoff_ms_ < kBackoffMaxMs) {
          backoff_ms_ = backoff_ms_ ? backoff_ms_ * 2 : kBackoffStartMs;
        }
        if (backoff_ms_ > kBackoffMaxMs) backoff_ms_ = kBackoffMaxMs;
        WiFi.begin(settings_.wifi_ssid, settings_.wifi_password);
      }
    }
  }
  accept_clients();
  pump_clients();
}

void WifiTransport::fill_status(StatusMessage* status) {
  switch (state_) {
    case State::OFF:
      status->wifi_state = meshpigeon_Status_WifiState_WIFI_STATE_OFF;
      break;
    case State::CONNECTING:
      status->wifi_state = meshpigeon_Status_WifiState_WIFI_STATE_CONNECTING;
      break;
    case State::CONNECTED:
      status->wifi_state = meshpigeon_Status_WifiState_WIFI_STATE_CONNECTED;
      break;
    case State::AUTH_FAIL:
      status->wifi_state = meshpigeon_Status_WifiState_WIFI_STATE_AUTH_FAIL;
      break;
    case State::ERROR:
      status->wifi_state = meshpigeon_Status_WifiState_WIFI_STATE_ERROR;
      break;
  }
  // The proto field is the *associated* SSID, empty when not associated —
  // the configured one is what GetDeviceSettings already reports, and the
  // two are not the same thing once the driver has latched onto something
  // other than what we asked for.
  status->wifi_ssid[0] = 0;
  status->wifi_port = port_;
  status->wifi_ipv4.size = 0;
  if (state_ == State::CONNECTED) {
    const String associated = WiFi.SSID();
    strncpy(status->wifi_ssid, associated.c_str(),
            sizeof(status->wifi_ssid) - 1);
    status->wifi_ssid[sizeof(status->wifi_ssid) - 1] = 0;
    IPAddress ip = WiFi.localIP();
    for (uint8_t i = 0; i < 4; i++) {
      status->wifi_ipv4.bytes[i] = ip[i];
    }
    status->wifi_ipv4.size = 4;
    status->wifi_rssi = WiFi.RSSI();
  } else {
    status->wifi_rssi = 0;  // unknown when not associated
  }
}

}  // namespace meshpigeon

#endif  // MESHPIGEON_ESP32 && MESHPIGEON_HAS_WIFI
