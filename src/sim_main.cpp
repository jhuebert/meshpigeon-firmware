/**
 * MeshPigeon desktop radio simulator.
 *
 * A stand-in for a real radio on the bench: runs the identical board-neutral
 * command core behind a TCP bridge, with scriptable RF loss/duplicate
 * behavior. The app's CI drives full pipelines against this instead of
 * hardware (09-testing §2).
 *
 * Build:  pio run -e sim
 * Run:    .pio/build/sim/meshpigeon-sim --port 8765 [--loss 10] [--dup 5]
 *              [--traffic-ms 3000] [--store 5000]
 */
#ifdef MESHPIGEON_NATIVE

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <vector>

#include "meshpigeon/command_processor.h"
#include "meshpigeon/framing.h"
#include "meshpigeon/sim_radio.h"

using namespace meshpigeon;

static const uint32_t kDefaultStoreBytes = 65536;

static ManualClock sim_clock(0);
static UptimeClock uptime(sim_clock);
static MemorySettingsStore sim_settings;
static SimRadio sim_radio;
static PacketStore* g_store = NULL;
static CommandProcessor* g_processor = NULL;

static CommandProcessor& processor() { return *g_processor; }

class Client : public IFrameSink {
 public:
  explicit Client(int fd) : fd_(fd) {}

  void send_frame(const uint8_t* decoded, size_t len) override {
    uint8_t wire[FRAME_MAX_WIRE];
    size_t n = frame_encode_wire(wire, decoded, len);
    ssize_t r = send(fd_, wire, n, MSG_NOSIGNAL);
    (void)r;
  }

  bool pump() {  // read socket -> feed reader -> dispatch frames
    uint8_t buf[1024];
    ssize_t n = recv(fd_, buf, sizeof(buf), MSG_DONTWAIT);
    if (n == 0) return false;  // closed
    if (n < 0) return true;    // no data right now
    for (ssize_t i = 0; i < n; i++) {
      size_t res = reader_.feed(buf[i], frame_);
      if (res != 0 && res != (size_t)-1) {
        processor().on_frame(frame_[0], frame_[1], frame_[2], frame_ + 3,
                             res - 5, this);
      }
    }
    return true;
  }

  int fd() const { return fd_; }

 private:
  int fd_;
  FrameReader reader_;
  uint8_t frame_[FRAME_MAX_DECODED];
};

static std::vector<Client*> clients;

static void sim_loop_tick(uint32_t traffic_ms) {
  sim_clock.advance(1);
  g_processor->poll();

  // Synthetic OTA traffic so connected apps see live packets.
  static uint32_t last_traffic = 0;
  if (traffic_ms > 0 && sim_clock.millis() - last_traffic >= traffic_ms) {
    last_traffic = sim_clock.millis();
    uint8_t pkt[64];
    pkt[0] = 0x45;  // flood TXT_MSG, 3-byte hashes — realistic shape
    uint32_t blob = sim_clock.millis();
    memcpy(&pkt[1], &blob, 4);
    const char* text = "sim: hello from the desktop mesh";
    memcpy(&pkt[5], text, strlen(text) + 1);
    uint8_t len = (uint8_t)(5 + strlen(text) + 1);
    sim_radio.inject(pkt, len);
  }

  // Radio loop: pull packets off the "air" into the store + live push.
  uint8_t raw[256];
  uint8_t len;
  int8_t rssi, snr;
  if (sim_radio.receive(raw, &len, &rssi, &snr)) {
    g_processor->on_packet_received(rssi, snr, raw, len);
  }
}

int main(int argc, char** argv) {
  uint16_t port = 8765;
  uint32_t traffic_ms = 0;
  uint32_t store_bytes = kDefaultStoreBytes;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--port") && i + 1 < argc) port = (uint16_t)atoi(argv[++i]);
    else if (!strcmp(argv[i], "--traffic-ms") && i + 1 < argc) traffic_ms = (uint32_t)atoi(argv[++i]);
    else if (!strcmp(argv[i], "--store") && i + 1 < argc) store_bytes = (uint32_t)atoi(argv[++i]);
    else if (!strcmp(argv[i], "--loss") && i + 1 < argc) sim_radio.set_loss((uint8_t)atoi(argv[++i]));
    else if (!strcmp(argv[i], "--dup") && i + 1 < argc) sim_radio.set_dup((uint8_t)atoi(argv[++i]));
    else if (!strcmp(argv[i], "--help")) {
      printf("usage: meshpigeon-sim [--port N] [--traffic-ms N] [--store BYTES] [--loss N] [--dup N]\n");
      return 0;
    }
  }

  g_store = new PacketStore(store_bytes);
  g_processor = new CommandProcessor(*g_store, uptime, sim_settings, sim_radio,
                                     "SIM", "0.1.0");
  g_processor->boot();

  signal(SIGPIPE, SIG_IGN);

  int server = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);
  if (bind(server, (sockaddr*)&addr, sizeof(addr)) != 0) {
    perror("bind");
    return 1;
  }
  listen(server, 4);
  printf("meshpigeon-sim: listening on tcp:%u (store=%u bytes, traffic=%ums)\n", port,
         store_bytes, traffic_ms);
  fflush(stdout);

  while (true) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(server, &fds);
    int maxfd = server;
    for (Client* c : clients) {
      FD_SET(c->fd(), &fds);
      if (c->fd() > maxfd) maxfd = c->fd();
    }
    timeval tv{0, 1000};  // 1 ms tick
    int ready = select(maxfd + 1, &fds, NULL, NULL, &tv);
    if (ready > 0) {
      if (FD_ISSET(server, &fds)) {
        int fd = accept(server, NULL, NULL);
        if (fd >= 0) {
          Client* c = new Client(fd);
          clients.push_back(c);
          g_processor->add_sink(c);
        }
      }
      for (size_t i = 0; i < clients.size();) {
        if (!clients[i]->pump()) {
          g_processor->remove_sink(clients[i]);
          close(clients[i]->fd());
          delete clients[i];
          clients.erase(clients.begin() + i);
        } else {
          i++;
        }
      }
    }
    sim_loop_tick(traffic_ms);
  }
}

#else
int main() { return 0; }
#endif  // MESHPIGEON_NATIVE
