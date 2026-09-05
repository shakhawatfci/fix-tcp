#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

int main() {

    int sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_in server{};

    server.sin_family = AF_INET;
    server.sin_port = htons(5001);

    inet_pton(
        AF_INET,
        "127.0.0.1",
        &server.sin_addr
    );

    if (connect(
            sock,
            reinterpret_cast<sockaddr*>(&server),
            sizeof(server)) < 0) {

        perror("connect");
        return 1;
    }

    std::cout << "Connected to server!\n";

    std::string message = "Hello from FIX client";

    ssize_t sent = send(
        sock,
        message.data(),
        message.size(),
        0
    );

    std::cout << "Sent " << sent << " bytes\n";

    close(sock);

    return 0;
}