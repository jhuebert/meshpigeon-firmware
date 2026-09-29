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
  if (client.write(wire, n) == 0) {
    // The socket is gone or its buffer is full; the board loop notices on
    // the next pump and drops this client.
    return;
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
  bool was_enabled = settings_.wifi_enabled;
  char prev_ssid[MESHPIGEON_SSID_MAX + 1];
  strncpy(prev_ssid, settings_.wifi_ssid, sizeof(prev_ssid));
  prev_ssid[sizeof(prev_ssid) - 1] = 0;
  char prev_pass[MESHPIGEON_PASS_MAX + 1];
  strncpy(prev_pass, settings_.wifi_password, sizeof(prev_pass));
  prev_pass[sizeof(prev_pass) - 1] = 0;
  uint16_t prev_port = port_;

  settings_ = settings;
  if (settings_.wifi_port == 0) settings_.wifi_port = MESHPIGEON_WIFI_PORT_DEFAULT;

  if (!settings_.wifi_enabled) {
    if (was_enabled) {
      stop_server();  // drops the sockets too
      disconnect();
    }
    return;
  }
  if (settings_.wifi_ssid[0] == 0) return;  // enabled but unconfigured: OFF

  bool credentials_changed = strcmp(prev_ssid, settings_.wifi_ssid) != 0 ||
                            strcmp(prev_pass, settings_.wifi_password) != 0;
  if (!was_enabled || credentials_changed) {
    // New network, new credentials, or newly enabled: rebind from scratch.
    stop_server();
    disconnect();
    backoff_ms_ = 0;
    last_attempt_ms_ = 0;
    port_ = settings_.wifi_port;
    connect_start();
    return;
  }
  if (settings_.wifi_port != prev_port) {
    port_ = settings_.wifi_port;
    if (server_up_) start_server();  // rebind the listener
  }
}

void WifiTransport::connect_start() {
  if (state_ == State::OFF) set_state(State::CONNECTING);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // the radio is the point; don't nap the link
  WiFi.begin(settings_.wifi_ssid, settings_.wifi_password);
  last_attempt_ms_ = millis();
  if (backoff_ms_ == 0) backoff_ms_ = kBackoffStartMs;
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
  while (num_clients_ > 0) drop_client(num_clients_ - 1);
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
    clients_[i].client.stop();
    clients_[i].client = c;
    clients_[i].reader.reset();
    clients_[i].in_use = true;
    clients_[i].authenticated = false;
    num_clients_++;
    if (proc_) proc_->add_sink(&clients_[i]);
    return;
  }
  // At capacity: the extra client is dropped rather than queued.
}

void WifiTransport::pump_clients() {
  for (size_t i = 0; i < kMaxClients; i++) {
    Client& c = clients_[i];
    if (!c.in_use) continue;
    while (c.client.available() > 0) {
      size_t res = c.reader.feed((uint8_t)c.client.read(), c.frame);
      if (res != 0 && res != (size_t)-1) {
        proc_->on_envelope(c.frame, res, &c);
      }
    }
    if (!c.client.connected()) {
      if (proc_) proc_->remove_sink(&c);
      c.client.stop();
      c.in_use = false;
      num_clients_--;
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
  if (!settings_.wifi_enabled || settings_.wifi_ssid[0] == 0) {
    // A disabled link closes everything it opened (docs/radio-protocol.md §7); the
    // disconnect path in apply() already did that.
    return;
  }
  if (state_ == State::CONNECTED) {
    if (WiFi.status() != WL_CONNECTED) {
      stop_server();
      set_state(State::CONNECTING);
      backoff_ms_ = 0;
    }
  }
  if (state_ == State::CONNECTING || state_ == State::AUTH_FAIL ||
      state_ == State::ERROR) {
    wl_status_t st = WiFi.status();
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
        if (backoff_ms_ < kBackoffMaxMs) backoff_ms_ *= 2;
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
  strncpy(status->wifi_ssid, settings_.wifi_ssid, sizeof(status->wifi_ssid) - 1);
  status->wifi_ssid[sizeof(status->wifi_ssid) - 1] = 0;
  status->wifi_port = port_;
  status->wifi_ipv4.size = 0;
  if (state_ == State::CONNECTED) {
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
