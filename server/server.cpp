#include <iostream>
#include <string>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>



constexpr int PORT = 8080;
constexpr int BUFFER_SIZE = 1024;

int main() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;

    socklen_t addrlen = sizeof(address);
    char buffer[BUFFER_SIZE] = {0};
    //create socket fd
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) { //using IPV4
        perror("socket failed");
        exit(EXIT_FAILURE);
    }
    //attach socket to a port
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*) &address, sizeof(address)) < 0) {
        perror("bind error");
        exit(EXIT_FAILURE);
    }

    std::cout << "Server listening on port " << PORT << std::endl;
    // accept the incoming connections

    if ((new_socket = accept(server_fd, (struct sockaddr*)&address, &addrlen)) < 0) {
        perror("accept error");
        exit(EXIT_FAILURE);
    }

    // Read and echo the received message
    ssize_t valread = read(new_socket, buffer, BUFFER_SIZE);
    std::cout << "Received: " << buffer << std::endl;
    send(new_socket, buffer, valread, 0);
    std::cout << "Echo message sent" << std::endl;
    // Close the socket
    close(new_socket);
    close(server_fd);
    return 0;
}