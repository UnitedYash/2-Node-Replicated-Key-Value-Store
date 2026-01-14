#include <iostream>
#include <string>
#include <cstring>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <thread>
constexpr int PORT = 8080;


bool handle_client(int new_socket, std::string& buffer) {
    char temp[1024];
    ssize_t valread = recv(new_socket, temp, sizeof(temp), 0);

    if (valread <= 0) {
        if (valread == 0) std::cout << "Client disconnected.\n";
        else perror("recv error");
        close(new_socket); 
        return false;
    }

    buffer.append(temp, valread);

    size_t pos;
    while ((pos = buffer.find('\n')) != std::string::npos) {
        std::string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);

        std::cout << "Command: " << line << std::endl;

        // For now: echo back
        std::string reply = line + "\n";
        send(new_socket, reply.c_str(), reply.size(), 0);
    }



    std::cout << "Received: " << std::string_view(buffer.data(), valread) << std::endl;

    send(new_socket, buffer.data(), valread, 0);
    return true; 
}

int main() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    socklen_t addrlen = sizeof(address);
    std::array<char, 1024> buffer;

    // 1. Create socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // 2. Allow reuse of address
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    // 3. Bind
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    // 4. Listen
    if (listen(server_fd, 3) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    std::cout << "Server listening on port " << PORT << std::endl;

    // 5. Accept loop
    while (1) {
        new_socket = accept(server_fd, (struct sockaddr*)&address, &addrlen);
        if (new_socket < 0) {
            perror("accept");
            continue;
        }

        std::cout << "Client connected!\n";
        std::thread([new_socket]() {
        std::string buffer;
        while (handle_client(new_socket, buffer)) {
                // keep serving this client
            }
        }).detach();
    }


    close(server_fd);
    return 0;
}
