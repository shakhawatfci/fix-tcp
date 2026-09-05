#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(5001);

    if (bind(server_fd,
             reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        return 1;
    }

    std::cout << "Server listening on port 5001...\n";

    int client_fd = accept(server_fd, nullptr, nullptr);

    if (client_fd < 0) {
        perror("accept");
        return 1;
    }

    std::cout << "Client connected!\n";

    char buffer[4096];

    while (true) {
        ssize_t bytes = recv(client_fd, buffer, sizeof(buffer), 0);

        if (bytes > 0) {
            std::cout << "Received " << bytes << " bytes:\n";
            std::cout.write(buffer, bytes);
            std::cout << "\n";
        }
        else if (bytes == 0) {
            std::cout << "Client disconnected\n";
            break;
        }
        else {
            perror("recv");
            break;
        }
    }

    close(client_fd);
    close(server_fd);

    return 0;
}