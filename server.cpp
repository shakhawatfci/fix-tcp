// ─── FIX Engine Server ────────────────────────────────────────────────────
// Build: g++ -std=c++17 -Wall -Wextra -O2 server.cpp -o server
// Accepts multiple clients (one at a time, sequential).
// Receives FIX messages, parses them, and replies:
//   • Logon  → responds with Logon
//   • Heartbeat → responds with Heartbeat
//   • Anything else → logs and ignores

#include <arpa/inet.h>
#include <csignal>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "fix_message.hpp"

static const int    PORT       = 5001;
static const char*  SENDER_ID  = "SERVER";

// ─── Send a complete FIX message over a socket ────────────────────────────
static bool sendFix(int fd, const FixMessage& msg) {
    std::string raw = msg.serialize();
    ssize_t sent = send(fd, raw.data(), raw.size(), 0);
    if (sent < 0) {
        perror("send");
        return false;
    }
    std::cout << "[TX] " << msg.pretty() << "\n" << std::flush;
    return true;
}

// ─── Handle one connected client session ──────────────────────────────────
static void handleClient(int client_fd, int& serverSeq) {
    std::cout << "Client connected (fd=" << client_fd << ")\n" << std::flush;

    char buf[65536];
    std::string pending;   // partial-message accumulator

    while (true) {
        ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
        if (n == 0) {
            std::cout << "Client disconnected gracefully.\n";
            break;
        }
        if (n < 0) {
            perror("recv");
            break;
        }

        pending.append(buf, n);

        // A FIX message ends with "10=<checksum>\x01"
        // We scan for that pattern to pull out complete messages.
        while (true) {
            // Find "10=" which signals the CheckSum field
            auto pos = pending.find("\x01" "10=");
            if (pos == std::string::npos) break;

            // Find the SOH after the checksum value
            auto eom = pending.find('\x01', pos + 4);
            if (eom == std::string::npos) break;

            std::string rawMsg = pending.substr(0, eom + 1);
            pending.erase(0, eom + 1);

            // ── Parse ──────────────────────────────────────────────────
            FixMessage incoming = FixMessage::parse(rawMsg);
            std::string msgType  = incoming.get(tag::MsgType,      "?");
            std::string clientId = incoming.get(tag::SenderCompID, "CLIENT");

            std::cout << "[RX] " << incoming.pretty() << "\n";

            // ── Reply ──────────────────────────────────────────────────
            if (msgType == msgtype::Logon) {
                std::cout << "[INFO] Logon received from " << clientId << "\n";
                FixMessage reply = buildLogon(SENDER_ID, clientId, ++serverSeq);
                sendFix(client_fd, reply);

            } else if (msgType == msgtype::Heartbeat) {
                std::cout << "[INFO] Heartbeat received from " << clientId << "\n";
                std::string testId = incoming.get(tag::TestReqID, "");
                FixMessage reply = buildHeartbeat(SENDER_ID, clientId,
                                                  ++serverSeq, testId);
                sendFix(client_fd, reply);

            } else if (msgType == msgtype::Logout) {
                std::cout << "[INFO] Logout received from " << clientId << "\n";
                break;

            } else {
                std::cout << "[INFO] Unknown MsgType=" << msgType << " – ignoring\n";
            }
        }
    }

    close(client_fd);
    std::cout << "Client session ended.\n\n";
}

// ─── Main ─────────────────────────────────────────────────────────────────
int main() {
    // Ignore SIGPIPE so a broken client doesn't kill the server
    signal(SIGPIPE, SIG_IGN);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    // Allow immediate restart without "Address already in use"
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(server_fd, 10) < 0) {
        perror("listen"); return 1;
    }

    // Disable stdout buffering so Docker captures output immediately
    std::cout.setf(std::ios::unitbuf);
    std::cout << "FIX Server listening on port " << PORT << " ...\n" << std::flush;

    int serverSeq = 0;

    // ── Accept loop: keeps running after each client disconnects ──────────
    while (true) {
        sockaddr_in clientAddr{};
        socklen_t   addrLen = sizeof(clientAddr);

        int client_fd = accept(server_fd,
                               reinterpret_cast<sockaddr*>(&clientAddr),
                               &addrLen);
        if (client_fd < 0) {
            perror("accept");
            continue;   // don't die, just wait for next client
        }

        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
        std::cout << "New connection from " << ipStr << "\n";

        handleClient(client_fd, serverSeq);
    }

    close(server_fd);
    return 0;
}