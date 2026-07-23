/* BASELINE RECEIVER (C) — naive on purpose. Rewrite it (C, C++, Go, or Rust).
 *
 * Ports (all 127.0.0.1):
 *   bind 47002  <- media from your sender, via the hostile relay
 *   send 47020  -> harness player. MUST be: 4-byte big-endian seq +
 *                  160-byte payload. Frame i counts only if it arrives
 *                  BEFORE its deadline t0 + DELAY_MS + i*20ms.
 *   send 47003  -> feedback to your sender, via the relay (optional)
 *
 * This baseline forwards whatever arrives straight to the player: lost
 * frames stay lost, late frames stay late, duplicates are re-sent
 * harmlessly. All yours to fix — jitter buffer, reordering, recovery.
 *
 * Env vars available: T0, DURATION_S, DELAY_MS. Harness kills the process
 * at run end; a forever-loop is fine.
 */
/*
 * BASELINE RECEIVER (C++) — naive on purpose.
 */

#include <iostream>
#include <cstring>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#include <cstdint>

using namespace std;

constexpr int PAYLOAD_BYTES = 160;

enum PacketType : uint8_t {
    PT_FRAME = 1,
    PT_PARITY = 2,
    PT_NACK = 3
};

struct Frame {
    bool received = false;
    uint32_t seq;
    unsigned char payload[PAYLOAD_BYTES];
};

struct Parity {
    bool valid = false;
    uint32_t block_start;
    uint8_t block_len;
    unsigned char parity[PAYLOAD_BYTES];
};

std::unordered_map<uint32_t, Frame> frame_buffer;
std::unordered_map<uint32_t, Parity> parity_buffer;

int main() {
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);

    sockaddr_in in_addr{};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (bind(in_fd, (sockaddr *)&in_addr, sizeof(in_addr)) < 0) {
        perror("bind 47002");
        return 1;
    }

    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    int fb_fd = socket(AF_INET, SOCK_DGRAM, 0);

    sockaddr_in feedback{};
    feedback.sin_family = AF_INET;
    feedback.sin_port = htons(47003);
    feedback.sin_addr.s_addr = inet_addr("127.0.0.1");

    sockaddr_in player{};
    player.sin_family = AF_INET;
    player.sin_port = htons(47020);
    player.sin_addr.s_addr = inet_addr("127.0.0.1");

    unsigned char buf[2048];

    while (true) {
        ssize_t n = recvfrom(in_fd, buf, sizeof(buf), 0, nullptr, nullptr);

        if (n <= 0)
            continue;

        // TODO:
        // 1. Reordering
        // 2. Jitter buffer
        // 3. Duplicate removal
        // 4. Recovery

    //     sendto(out_fd,
    //            buf,
    //            static_cast<size_t>(n),
    //            0,
    //            (sockaddr *)&player,
    //            sizeof(player));
        uint8_t type = buf[0];

        if(type == PT_FRAME)
        {
            uint32_t seq_be;
            memcpy(&seq_be, buf + 1, 4);

            uint32_t seq = ntohl(seq_be);

            Frame f;
            f.received = true;
            f.seq = seq;

            memcpy(f.payload, buf + 5, PAYLOAD_BYTES);

            frame_buffer[seq] = f;

            cout << "Frame " << seq << " received\n";
        }
        else if(type == PT_PARITY)
        {
            uint32_t start_be;

            memcpy(&start_be, buf + 1, 4);

            uint32_t block_start = ntohl(start_be);

            uint8_t len = buf[5];

            Parity p;

            p.valid = true;
            p.block_start = block_start;
            p.block_len = len;

            memcpy(p.parity, buf + 6, PAYLOAD_BYTES);

            parity_buffer[block_start] = p;

            cout << "Parity block " << block_start << endl;
        }
    }

    close(in_fd);
    close(out_fd);

    return 0;
}