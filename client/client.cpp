#include <iostream>
#include <string>
#include <cstring>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

constexpr int PORT = 8080;
constexpr int BUFFER_SIZE = 1024;

int main() {
    int sock = 0;
    struct sockaddr_in serv_addr;
    char buffer[BUFFER_SIZE] = {0};

    // 1. Create socket
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        std::cerr << "Socket creation error\n";
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    // 2. Convert address
    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
        std::cerr << "Invalid address\n";
        return -1;
    }

    // 3. Connect ONCE
    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << "Connection Failed\n";
        return -1;
    }

    std::cout << "Connected to server.\n";

    // 4. Send/receive loop
    while (true) {
        std::string msg;
        std::cout << "Enter message: ";
        std::getline(std::cin, msg);
        
        msg += "\n";
        send(sock, msg.c_str(), msg.size(), 0);

        memset(buffer, 0, BUFFER_SIZE);
        ssize_t bytes = read(sock, buffer, BUFFER_SIZE);
        if (bytes <= 0) {
            std::cout << "Server closed connection.\n";
            break;
        }

        std::cout << "Received: " << buffer << std::endl;
    }

    // 5. Close
    close(sock);
    return 0;
}
