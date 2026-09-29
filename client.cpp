// ─── FIX Engine Client ────────────────────────────────────────────────────
// Connects to the FIX server (hostname passed as argv[1]),
// sends Logon, then sends Heartbeat every 30 seconds.
// Receives and parses any replies from the server.

#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "fix_message.hpp"

static const int    PORT        = 5001;
static const char*  SENDER_ID   = "CLIENT";
static const char*  TARGET_ID   = "SERVER";
static const int    HB_INTERVAL = 30;   // seconds

static const std::string LOG_DIR      = "logs/clients";
static const std::string IN_LOG_PATH  = LOG_DIR + "/in.log";
static const std::string OUT_LOG_PATH = LOG_DIR + "/out.log";

static std::ofstream g_inLog;
static std::ofstream g_outLog;

static std::string utcLogTime() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H:%M:%S", &tm);
    char out[48];
    snprintf(out, sizeof(out), "%s.%03d", buf, static_cast<int>(ms.count()));
    return std::string(out);
}

static void initLogFiles() {
    std::error_code ec;
    std::filesystem::create_directories(LOG_DIR, ec);
    if (ec) {
        std::cerr << "Failed to create log directory " << LOG_DIR << ": " << ec.message() << "\n";
    }
    g_inLog.open(IN_LOG_PATH, std::ios::out | std::ios::app);
    if (!g_inLog.is_open()) {
        std::cerr << "Failed to open " << IN_LOG_PATH << " for writing\n";
    }
    g_outLog.open(OUT_LOG_PATH, std::ios::out | std::ios::app);
    if (!g_outLog.is_open()) {
        std::cerr << "Failed to open " << OUT_LOG_PATH << " for writing\n";
    }
}

static void logInMessage(const std::string& rawMsg) {
    if (g_inLog.is_open()) {
        g_inLog << utcLogTime() << "  " << rawMsg << "\n" << std::flush;
    }
}

static void logOutMessage(const std::string& rawMsg) {
    if (g_outLog.is_open()) {
        g_outLog << utcLogTime() << "  " << rawMsg << "\n" << std::flush;
    }
}

static volatile bool running = true;

void sigHandler(int) { running = false; }

// Log helper that always flushes
template<typename T>
static void LOG(const T& msg) {
    std::cout << msg << "\n" << std::flush;
}

// ─── Resolve hostname → IP, then connect ──────────────────────────────────
static int connectToServer(const char* hostname) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(hostname, std::to_string(PORT).c_str(), &hints, &res);
    if (rc != 0) {
        std::cerr << "getaddrinfo: " << gai_strerror(rc) << "\n";
        return -1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); freeaddrinfo(res); return -1; }

    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        perror("connect"); close(sock); freeaddrinfo(res); return -1;
    }

    freeaddrinfo(res);
    std::cout << "Connected to " << hostname << ":" << PORT << "\n";
    return sock;
}

// ─── Send a FIX message ───────────────────────────────────────────────────
static bool sendFix(int fd, const FixMessage& msg) {
    std::string raw = msg.serialize();
    ssize_t sent = send(fd, raw.data(), raw.size(), 0);
    if (sent < 0) { perror("send"); return false; }
    logOutMessage(raw);
    std::cout << "[TX] " << msg.pretty() << "\n";
    return true;
}

// ─── Main ─────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    // Force line-buffered output so Docker captures logs in real time
    std::cout.setf(std::ios::unitbuf);

    signal(SIGINT,  sigHandler);
    signal(SIGTERM, sigHandler);

    initLogFiles();

    const char* serverHost = (argc > 1) ? argv[1] : "fix-server";

    // Retry connection until server is ready
    int sock = -1;
    while (running && sock < 0) {
        sock = connectToServer(serverHost);
        if (sock < 0) {
            LOG("Retrying in 3s...");
            sleep(3);
        }
    }
    if (sock < 0) return 1;

    int clientSeq = 0;

    // ── Step 1: Send Logon ────────────────────────────────────────────────
    FixMessage logon = buildLogon(SENDER_ID, TARGET_ID, ++clientSeq, HB_INTERVAL);
    if (!sendFix(sock, logon)) { close(sock); return 1; }

    // ── Main loop: send Heartbeat every HB_INTERVAL, recv with select ─────
    std::string pending;
    time_t lastHb = time(nullptr);

    while (running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);

        // Wake up at least every second to check heartbeat timer
        struct timeval tv{1, 0};
        int sel = select(sock + 1, &rfds, nullptr, nullptr, &tv);

        // ── Send heartbeat if interval elapsed ────────────────────────
        time_t now = time(nullptr);
        if (now - lastHb >= HB_INTERVAL) {
            std::cout << "[INFO] Sending Heartbeat\n";
            FixMessage hb = buildHeartbeat(SENDER_ID, TARGET_ID, ++clientSeq);
            if (!sendFix(sock, hb)) break;
            lastHb = now;
        }

        if (sel < 0) { if (errno == EINTR) continue; perror("select"); break; }
        if (sel == 0) continue;  // timeout, loop again

        // ── Data available to read ────────────────────────────────────
        char buf[65536];
        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        if (n == 0) { std::cout << "Server disconnected.\n"; break; }
        if (n < 0)  { perror("recv"); break; }

        pending.append(buf, n);

        // Extract complete FIX messages (same delimiter logic as server)
        while (true) {
            auto pos = pending.find("\x01" "10=");
            if (pos == std::string::npos) break;

            auto eom = pending.find('\x01', pos + 4);
            if (eom == std::string::npos) break;

            std::string rawMsg = pending.substr(0, eom + 1);
            pending.erase(0, eom + 1);

            logInMessage(rawMsg);

            FixMessage reply = FixMessage::parse(rawMsg);
            std::string msgType = reply.get(tag::MsgType, "?");
            std::cout << "[RX] " << reply.pretty() << "\n";

            if (msgType == msgtype::Logon) {
                std::cout << "[INFO] Logon ACK received from server\n";
            } else if (msgType == msgtype::Heartbeat) {
                std::cout << "[INFO] Heartbeat received from server\n";
            } else if (msgType == msgtype::Logout) {
                std::cout << "[INFO] Logout from server – closing\n";
                running = false;
            }
        }
    }

    // ── Clean shutdown: send Logout ────────────────────────────────────────
    std::cout << "Sending Logout and closing...\n";
    FixMessage logout;
    logout.set(tag::BeginString,  "FIX.4.4");
    logout.set(tag::MsgType,      msgtype::Logout);
    logout.set(tag::SenderCompID, SENDER_ID);
    logout.set(tag::TargetCompID, TARGET_ID);
    logout.set(tag::MsgSeqNum,    ++clientSeq);
    logout.set(tag::SendingTime,  fixTimestamp());
    sendFix(sock, logout);

    close(sock);
    if (g_inLog.is_open())  g_inLog.close();
    if (g_outLog.is_open()) g_outLog.close();
    return 0;
}