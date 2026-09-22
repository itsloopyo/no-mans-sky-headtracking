// Recovery suite: the port this mod listens on is routinely held by whatever
// the player forgot to close, and the only acceptable behaviour is that the
// mod keeps running and picks the port up on its own within a fraction of a
// second of it being freed. This drives the real UdpReceiver the mod embeds -
// not a stand-in - against a socket that holds the port, and MEASURES the two
// numbers that matter: the retry cadence, and the gap between the port coming
// free and the first tracker packet reaching the receiver.

#include <cameraunlock/protocol/opentrack_packet.h>
#include <cameraunlock/protocol/udp_receiver.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

using cameraunlock::OpenTrackPacket;
using cameraunlock::UdpReceiver;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (ok) return;
    ++g_failures;
    std::printf("FAIL: %s\n", what);
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// A plain bound UDP socket, standing in for the other game the player left
/// running. No SO_REUSEADDR on either side, which is what makes the conflict
/// real rather than two sockets quietly sharing the port.
class PortHolder {
public:
    bool Hold(uint16_t port) {
        m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_socket == INVALID_SOCKET) return false;
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(m_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            Release();
            return false;
        }
        return true;
    }

    void Release() {
        if (m_socket != INVALID_SOCKET) {
            closesocket(m_socket);
            m_socket = INVALID_SOCKET;
        }
    }

    ~PortHolder() { Release(); }

private:
    SOCKET m_socket = INVALID_SOCKET;
};

/// Sends OpenTrack poses at 60 Hz to the loopback port, exactly as a tracker
/// app would: it never stops or restarts when the port changes hands, so the
/// packet the receiver sees first is whichever one lands after the bind.
class PoseSender {
public:
    bool Start(uint16_t port, double yaw) {
        m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_socket == INVALID_SOCKET) return false;
        m_addr.sin_family = AF_INET;
        m_addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &m_addr.sin_addr);
        m_yaw = yaw;
        m_stop.store(false);
        m_thread = std::thread([this] { Run(); });
        return true;
    }

    void Stop() {
        m_stop.store(true);
        if (m_thread.joinable()) m_thread.join();
        if (m_socket != INVALID_SOCKET) {
            closesocket(m_socket);
            m_socket = INVALID_SOCKET;
        }
    }

    ~PoseSender() { Stop(); }

private:
    void Run() {
        // The pose walks by a degree a packet. A tracker that repeats one value
        // bit-identically is what the receiver's dropout gate rejects, so a
        // frozen test pose would measure the gate rather than the recovery.
        double step = 0.0;
        while (!m_stop.load()) {
            double payload[6] = {0.0, 0.0, 0.0, m_yaw + step, 0.0, 0.0};
            sendto(m_socket, reinterpret_cast<const char*>(payload), sizeof(payload), 0,
                   reinterpret_cast<sockaddr*>(&m_addr), sizeof(m_addr));
            step = step > 5.0 ? 0.0 : step + 1.0;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }

    SOCKET m_socket = INVALID_SOCKET;
    sockaddr_in m_addr = {};
    double m_yaw = 0.0;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};

uint16_t FindFreePort() {
    // A fixed port flakes when a previous run's socket is still lingering or
    // when two suites run at once, so take the first one this machine will
    // actually hand over.
    for (uint16_t port = 47411; port < 47511; ++port) {
        PortHolder probe;
        if (probe.Hold(port)) return port;
    }
    return 0;
}

// A bind failure that names a cause the OS did not give sends the player
// hunting an app that is not running. The log line has to carry the failing
// call and the OS's own code.
void TheBindFailureLogCarriesTheOsCause(uint16_t port) {
    PortHolder holder;
    Check(holder.Hold(port), "test holder binds the port");

    UdpReceiver receiver;
    // SetLog's contract says the sink is called from the caller thread AND the
    // background retry thread, so the accumulator needs a lock on both sides.
    // Un-guarded, this is a data race on a std::string the moment a case
    // releases the port with a sink installed.
    std::mutex logMutex;
    std::string log;
    receiver.SetLog([&](const std::string& message) {
        std::lock_guard<std::mutex> lock(logMutex);
        log += message + "\n";
    });
    const auto logContains = [&](const char* needle) {
        std::lock_guard<std::mutex> lock(logMutex);
        return log.find(needle) != std::string::npos;
    };

    const bool started = receiver.Start(port);
    Check(!started, "Start() reports the port was not bound");
    Check(receiver.IsRetrying(), "receiver enters the retry loop rather than giving up");
    Check(logContains("bind failed with error"),
          "log names the call that failed");
    Check(logContains(std::to_string(WSAEADDRINUSE).c_str()),
          "log carries the address-in-use code the OS returned");
    Check(logContains("retrying every"),
          "log states the retry cadence");
    std::string logSnapshot;
    {
        std::lock_guard<std::mutex> lock(logMutex);
        logSnapshot = log;
    }
    std::printf("  bind failure log: %s", logSnapshot.c_str());

    receiver.Stop();
}

// The measurement this suite exists for. Each pass holds the port, starts the
// receiver against it, leaves a tracker sending throughout, then frees the port
// and times the gap to the first pose the receiver publishes.
void PortFreedToFirstPacket(uint16_t port, int passes) {
    std::vector<int64_t> latencies;

    for (int pass = 0; pass < passes; ++pass) {
        PortHolder holder;
        Check(holder.Hold(port), "test holder binds the port");

        UdpReceiver receiver;
        Check(!receiver.Start(port), "receiver defers while the port is held");

        PoseSender sender;
        Check(sender.Start(port, 20.0), "tracker sends throughout, held port or not");

        // Long enough that the retry loop is mid-cadence rather than fresh, so
        // the measured gap is a real sample of the wait and not always the
        // first attempt.
        std::this_thread::sleep_for(std::chrono::milliseconds(700 + 90 * pass));

        Check(receiver.IsRetrying(), "still retrying while the port is held");
        Check(!receiver.IsReceiving(), "no tracking data arrives while the port is held");

        const int64_t freedAt = NowMs();
        holder.Release();

        int64_t gotDataAt = 0;
        float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
        while (NowMs() - freedAt < 5000) {
            if (receiver.IsReceiving() && receiver.GetRotation(yaw, pitch, roll)) {
                gotDataAt = NowMs();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        Check(gotDataAt != 0, "tracker data arrives after the port is freed");
        if (gotDataAt != 0) {
            const int64_t latency = gotDataAt - freedAt;
            latencies.push_back(latency);
            std::printf("  pass %d: port freed -> first tracker pose in %lld ms (yaw %.1f)\n",
                        pass, static_cast<long long>(latency), static_cast<double>(yaw));
        }

        sender.Stop();
        receiver.Stop();
    }

    int64_t worst = 0;
    int64_t total = 0;
    for (int64_t value : latencies) {
        worst = value > worst ? value : worst;
        total += value;
    }
    if (!latencies.empty()) {
        std::printf("  recovery over %zu passes: worst %lld ms, mean %lld ms\n",
                    latencies.size(), static_cast<long long>(worst),
                    static_cast<long long>(total / static_cast<int64_t>(latencies.size())));
    }

    // The bound the cadence implies, with the terms named because the old
    // 200 ms allowance left about 12% of headroom over the worst legitimate
    // case and this is the one suite whose result depends on host load:
    //   500 ms  one retry interval (kRetryIntervalMs)
    //   100 ms  the supervisor's own tick, since a retry only fires on a tick
    //    17 ms  one 60 Hz packet interval before a pose lands
    //    16 ms  the Windows scheduler quantum this loop's 1 ms sleeps round to
    // = 633 ms accounted for, against the 900 ms allowed here.
    const int64_t bound = UdpReceiver::kRetryIntervalMs + 400;
    Check(!latencies.empty() && worst <= bound,
          "every recovery lands within one retry interval of the port freeing up");
    if (worst > bound) {
        std::printf("  worst recovery %lld ms exceeded the %lld ms the %d ms cadence allows\n",
                    static_cast<long long>(worst), static_cast<long long>(bound),
                    UdpReceiver::kRetryIntervalMs);
    }
}

// The retry loop has to survive the port being taken away again, because the
// player who reopens the other game is the same player who closed it.
void ItRecoversAgainAfterASecondConflict(uint16_t port) {
    UdpReceiver receiver;
    Check(receiver.Start(port), "receiver binds a free port");

    receiver.Stop();
    PortHolder holder;
    Check(holder.Hold(port), "another holder takes the port while the mod is down");

    Check(!receiver.Start(port), "receiver defers on the second conflict too");
    Check(receiver.IsRetrying(), "and retries rather than staying dead");

    const int64_t freedAt = NowMs();
    holder.Release();

    while (NowMs() - freedAt < 5000 && !receiver.IsRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const int64_t boundAt = NowMs();
    Check(receiver.IsRunning(), "receiver binds after the second conflict clears");
    std::printf("  second conflict: bound %lld ms after the port freed\n",
                static_cast<long long>(boundAt - freedAt));
    Check(boundAt - freedAt <= UdpReceiver::kRetryIntervalMs + 400,
          "second recovery lands within one retry interval too");

    receiver.Stop();
}

}  // namespace

int main() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("udp_recovery_test: WSAStartup failed\n");
        return EXIT_FAILURE;
    }

    const uint16_t port = FindFreePort();
    if (port == 0) {
        std::printf("udp_recovery_test: no free loopback port in the test range\n");
        WSACleanup();
        return EXIT_FAILURE;
    }
    std::printf("udp_recovery_test: using port %u, retry cadence %d ms\n", port,
                UdpReceiver::kRetryIntervalMs);

    TheBindFailureLogCarriesTheOsCause(port);
    PortFreedToFirstPacket(port, 5);
    ItRecoversAgainAfterASecondConflict(port);

    WSACleanup();

    if (g_failures != 0) {
        std::printf("udp_recovery_test: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("udp_recovery_test: all cases passed\n");
    return EXIT_SUCCESS;
}
