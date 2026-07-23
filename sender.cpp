/* SENDER (C++) — budgeted immediate duplicate + limited NACK retransmit.
 *
 * Wire format (first byte = type):
 *   PT_FRAME (1): [1][seq:4 BE][payload:160]
 *   PT_NACK  (3): [3][seq:4 BE]   receiver -> 47003
 *
 * At 2% loss and 40 ms playout delay, end-of-block FEC and NACK RTTs are too
 * slow. We spend the 2x bandwidth cap on ~94% immediate second copies.
 */
#include <arpa/inet.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

constexpr int PAYLOAD_BYTES = 160;
constexpr int RING_SIZE = 2048;
constexpr int DUP_PERIOD = 1500;
constexpr int DUP_QUOTA = 1407;   // ~1.998x uplink — leaves room for NACK retransmits

enum PktType : uint8_t { PT_FRAME = 1, PT_NACK = 3 };

struct RingEntry {
    uint32_t seq = 0xFFFFFFFFu;
    unsigned char payload[PAYLOAD_BYTES];
};

RingEntry g_ring[RING_SIZE];
std::mutex g_ring_mtx;

int g_out_fd = -1;
sockaddr_in g_relay_addr{};

bool send_duplicate(uint32_t seq) {
    return (seq % DUP_PERIOD) < static_cast<uint32_t>(DUP_QUOTA);
}

void send_frame(uint32_t seq, const unsigned char *payload) {
    unsigned char buf[1 + 4 + PAYLOAD_BYTES];
    buf[0] = PT_FRAME;
    uint32_t seq_be = htonl(seq);
    memcpy(buf + 1, &seq_be, 4);
    memcpy(buf + 5, payload, PAYLOAD_BYTES);
    sendto(g_out_fd, buf, sizeof buf, 0,
           reinterpret_cast<sockaddr *>(&g_relay_addr), sizeof g_relay_addr);
}

void store_frame(uint32_t seq, const unsigned char *payload) {
    std::lock_guard<std::mutex> lk(g_ring_mtx);
    RingEntry &e = g_ring[seq % RING_SIZE];
    e.seq = seq;
    memcpy(e.payload, payload, PAYLOAD_BYTES);
}

bool lookup_frame(uint32_t seq, unsigned char *out) {
    std::lock_guard<std::mutex> lk(g_ring_mtx);
    RingEntry &e = g_ring[seq % RING_SIZE];
    if (e.seq == seq) {
        memcpy(out, e.payload, PAYLOAD_BYTES);
        return true;
    }
    return false;
}

void feedback_loop() {
    int fb_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in fb_addr{};
    fb_addr.sin_family = AF_INET;
    fb_addr.sin_port = htons(47004);
    fb_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(fb_fd, reinterpret_cast<sockaddr *>(&fb_addr), sizeof fb_addr) < 0) {
        perror("bind 47004");
        return;
    }
    unsigned char buf[64];
    for (;;) {
        ssize_t n = recvfrom(fb_fd, buf, sizeof buf, 0, nullptr, nullptr);
        if (n < 5 || buf[0] != PT_NACK) continue;
        uint32_t seq_be;
        memcpy(&seq_be, buf + 1, 4);
        uint32_t seq = ntohl(seq_be);
        unsigned char payload[PAYLOAD_BYTES];
        if (lookup_frame(seq, payload)) send_frame(seq, payload);
    }
}

}  // namespace

int main() {
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in in_addr{};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47010);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, reinterpret_cast<sockaddr *>(&in_addr), sizeof in_addr) < 0) {
        perror("bind 47010");
        return 1;
    }

    g_out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    g_relay_addr.sin_family = AF_INET;
    g_relay_addr.sin_port = htons(47001);
    g_relay_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    std::thread fb_thread(feedback_loop);
    fb_thread.detach();

    unsigned char buf[2048];
    for (;;) {
        ssize_t n = recvfrom(in_fd, buf, sizeof buf, 0, nullptr, nullptr);
        if (n < static_cast<ssize_t>(4 + PAYLOAD_BYTES)) continue;

        uint32_t seq_be;
        memcpy(&seq_be, buf, 4);
        uint32_t seq = ntohl(seq_be);
        const unsigned char *payload = buf + 4;

        store_frame(seq, payload);
        send_frame(seq, payload);
        if (send_duplicate(seq)) send_frame(seq, payload);
    }
    return 0;
}
