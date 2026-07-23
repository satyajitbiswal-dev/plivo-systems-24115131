/* RECEIVER (C++) — reorder buffer, dedup, deadline timer, NACK fallback. */

#include <arpa/inet.h>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>

using namespace std::chrono;

constexpr int PAYLOAD_BYTES = 160;
constexpr double FRAME_S = 0.020;
constexpr double NACK_RTT_S = 0.090;
constexpr double NACK_INTERVAL_S = 0.020;
constexpr int DUP_PERIOD = 1500;
constexpr int DUP_QUOTA = 1407;
constexpr int MAX_NACKS_PER_SEQ = 2;

enum PacketType : uint8_t
{
    PT_FRAME = 1,
    PT_NACK = 3
};

struct Frame
{
    uint32_t seq;
    unsigned char payload[PAYLOAD_BYTES];
};

double g_t0 = 0.0;
double g_delay_ms = 0.0;
uint32_t expected_seq = 0;

std::unordered_map<uint32_t, Frame> frame_buffer;

uint32_t last_nack_seq = UINT32_MAX;
double last_nack_time = 0.0;
int nack_count_for_seq = 0;

bool frame_got_duplicate(uint32_t seq) {
    return (seq % DUP_PERIOD) < static_cast<uint32_t>(DUP_QUOTA);
}

double now_epoch()
{
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

double deadline_for(uint32_t seq)
{
    return g_t0 + g_delay_ms / 1000.0 + seq * FRAME_S;
}

void send_nack(int nack_fd, const sockaddr_in &relay, uint32_t seq)
{
    unsigned char buf[5];
    buf[0] = PT_NACK;
    uint32_t seq_be = htonl(seq);
    memcpy(buf + 1, &seq_be, 4);
    sendto(nack_fd, buf, sizeof(buf), 0,
           reinterpret_cast<const sockaddr *>(&relay), sizeof(relay));
}

void trim_buffers()
{
    for (auto it = frame_buffer.begin(); it != frame_buffer.end();)
    {
        if (it->first < expected_seq)
            it = frame_buffer.erase(it);
        else
            ++it;
    }
}

void advance_playout(int out_fd, const sockaddr_in &player,
                     int nack_fd, const sockaddr_in &relay)
{
    while (true)
    {
        auto it = frame_buffer.find(expected_seq);
        if (it != frame_buffer.end())
        {
            unsigned char out[4 + PAYLOAD_BYTES];
            uint32_t out_seq_be = htonl(it->second.seq);
            memcpy(out, &out_seq_be, 4);
            memcpy(out + 4, it->second.payload, PAYLOAD_BYTES);
            sendto(out_fd, out, sizeof(out), 0,
                   reinterpret_cast<const sockaddr *>(&player), sizeof(player));
            frame_buffer.erase(it);
            expected_seq++;
            nack_count_for_seq = 0;
            continue;
        }

        double now = now_epoch();
        double deadline = deadline_for(expected_seq);
        if (now >= deadline)
        {
            expected_seq++;
            nack_count_for_seq = 0;
            continue;
        }

        if (!frame_got_duplicate(expected_seq) &&
            now + NACK_RTT_S < deadline &&
            nack_count_for_seq < MAX_NACKS_PER_SEQ)
        {
            if (last_nack_seq != expected_seq)
            {
                last_nack_seq = expected_seq;
                nack_count_for_seq = 0;
            }
            if (now - last_nack_time >= NACK_INTERVAL_S)
            {
                send_nack(nack_fd, relay, expected_seq);
                last_nack_time = now;
                nack_count_for_seq++;
            }
        }
        break;
    }
    trim_buffers();
}

int poll_timeout_ms()
{
    double now = now_epoch();
    double deadline = deadline_for(expected_seq);
    double wait_s = deadline - now;
    if (wait_s <= 0.0)
        return 0;
    if (wait_s > 0.050)
        wait_s = 0.050;
    return static_cast<int>(wait_s * 1000.0);
}

void handle_frame(const unsigned char *buf, ssize_t n)
{
    if (n < 5 + PAYLOAD_BYTES)
        return;

    uint32_t seq_be;
    memcpy(&seq_be, buf + 1, 4);
    uint32_t seq = ntohl(seq_be);

    if (seq < expected_seq || frame_buffer.count(seq))
        return;

    Frame f;
    f.seq = seq;
    memcpy(f.payload, buf + 5, PAYLOAD_BYTES);
    frame_buffer[seq] = f;
}

int main()
{
    const char *t0_env = getenv("T0");
    const char *delay_env = getenv("DELAY_MS");
    if (!t0_env || !delay_env)
    {
        std::cerr << "missing T0 or DELAY_MS env\n";
        return 1;
    }
    g_t0 = atof(t0_env);
    g_delay_ms = atof(delay_env);

    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in in_addr{};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, reinterpret_cast<sockaddr *>(&in_addr), sizeof(in_addr)) < 0)
    {
        perror("bind 47002");
        return 1;
    }

    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in player{};
    player.sin_family = AF_INET;
    player.sin_port = htons(47020);
    player.sin_addr.s_addr = inet_addr("127.0.0.1");

    int nack_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in relay{};
    relay.sin_family = AF_INET;
    relay.sin_port = htons(47003);
    relay.sin_addr.s_addr = inet_addr("127.0.0.1");

    unsigned char buf[2048];
    pollfd pfd{in_fd, POLLIN, 0};

    while (true)
    {
        advance_playout(out_fd, player, nack_fd, relay);

        int timeout_ms = poll_timeout_ms();
        int ready = poll(&pfd, 1, timeout_ms);
        if (ready <= 0)
            continue;

        ssize_t n = recvfrom(in_fd, buf, sizeof(buf), 0, nullptr, nullptr);
        if (n <= 0)
            continue;

        if (buf[0] == PT_FRAME)
            handle_frame(buf, n);
    }

    close(in_fd);
    close(out_fd);
    close(nack_fd);
    return 0;
}
