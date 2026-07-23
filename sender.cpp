/* SENDER (C++) — block-XOR FEC + NACK retransmit fallback.
 *
 * Wire protocol sender<->receiver (our own design, first byte = type):
 *   PT_FRAME  (1): [1][seq:4 BE][payload:160]                = 165 B
 *       One media frame. Sent immediately on receipt from the harness,
 *       and again (unchanged) if a NACK asks for it.
 *   PT_PARITY (2): [2][block_start:4 BE][block_len:1][xor:160] = 166 B
 *       XOR of `block_len` consecutive frame payloads starting at
 *       block_start. Lets the receiver reconstruct exactly ONE missing
 *       frame per block without a round trip.
 *   PT_NACK   (3), receiver -> sender via 47003 -> relay -> 47004:
 *       [3][seq:4 BE] = 5 B. "I'm still missing this frame and there's
 *       time left before its deadline — please resend."
 *
 * Ports (all 127.0.0.1):
 *   bind 47010  <- harness source (seq:4 BE + 160B payload, every 20ms)
 *   send 47001  -> relay uplink (our protocol above)
 *   bind 47004  <- feedback from receiver, via relay (NACKs)
 *
 * Tune BLOCK_LEN: smaller = faster recovery, more overhead.
 *   BLOCK_LEN=5 -> ~1.24x overhead, worst-case extra recovery latency
 *                  ~ BLOCK_LEN*20ms if the lost frame was first in block.
 *
 * build: g++ -O2 -Wall -std=c++17 -pthread -o sender sender.cpp
 */
#include <arpa/inet.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

constexpr int PAYLOAD_BYTES = 160;
constexpr int RING_SIZE = 2048;      // ~40s of history at 20ms/frame
constexpr uint32_t BLOCK_LEN = 5;    // frames per FEC parity group — tune me

enum PktType : uint8_t { PT_FRAME = 1, PT_PARITY = 2, PT_NACK = 3 };

struct RingEntry {
    uint32_t seq = 0xFFFFFFFFu;      // sentinel = empty slot
    unsigned char payload[PAYLOAD_BYTES];
};

RingEntry g_ring[RING_SIZE];
std::mutex g_ring_mtx;

int g_out_fd = -1;
sockaddr_in g_relay_addr{};

void send_frame(uint32_t seq, const unsigned char *payload) {
    unsigned char buf[1 + 4 + PAYLOAD_BYTES];
    buf[0] = PT_FRAME;
    uint32_t seq_be = htonl(seq);
    memcpy(buf + 1, &seq_be, 4);
    memcpy(buf + 5, payload, PAYLOAD_BYTES);
    sendto(g_out_fd, buf, sizeof buf, 0,
           reinterpret_cast<sockaddr *>(&g_relay_addr), sizeof g_relay_addr);
}

void send_parity(uint32_t block_start, uint8_t block_len,
                  const unsigned char *xor_payload) {
    unsigned char buf[1 + 4 + 1 + PAYLOAD_BYTES];
    buf[0] = PT_PARITY;
    uint32_t bs_be = htonl(block_start);
    memcpy(buf + 1, &bs_be, 4);
    buf[5] = block_len;
    memcpy(buf + 6, xor_payload, PAYLOAD_BYTES);
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

// Listens for NACKs from the receiver (via the relay) and resends the
// exact frame requested, if we still have it.
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

    unsigned char block_xor[PAYLOAD_BYTES];
    bool block_open = false;
    uint32_t block_start = 0;
    uint8_t block_count = 0;

    unsigned char buf[2048];
    for (;;) {
        ssize_t n = recvfrom(in_fd, buf, sizeof buf, 0, nullptr, nullptr);
        if (n < static_cast<ssize_t>(4 + PAYLOAD_BYTES)) continue;

        uint32_t seq_be;
        memcpy(&seq_be, buf, 4);
        uint32_t seq = ntohl(seq_be);
        const unsigned char *payload = buf + 4;

        store_frame(seq, payload);
        send_frame(seq, payload);  // immediate, lowest-latency copy

        if (!block_open) {
            block_open = true;
            block_start = seq;
            block_count = 0;
            memset(block_xor, 0, PAYLOAD_BYTES);
        }
        for (int i = 0; i < PAYLOAD_BYTES; i++) block_xor[i] ^= payload[i];
        block_count++;
        if (block_count >= BLOCK_LEN) {
            send_parity(block_start, block_count, block_xor);
            block_open = false;
        }
    }
    return 0;
}
