#include <iostream>
#include <string>
#include <cstring>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sstream>
#include <unistd.h>
#include <thread>
#include <unordered_map>
#include <mutex>

std::unordered_map<std::string, std::string> store;
std::mutex store_mutex;
constexpr int PORT = 8080;


void handle_put(int client_fd, const std::string& key, const std::string& value) {
    {
        //get a lock to put the key
        std::lock_guard<std::mutex> lock(store_mutex);
        store[key] = value;
    }
    //lock releases here

    std::string ok = "OK\n";
    send(client_fd, ok.c_str(), ok.size(), 0);
}

std::string read_exactly(int fd, size_t length,
                         std::string& buffer) {
    std::string result;

    // 1. Consume from existing buffer first
    size_t take = std::min(length, buffer.size());
    result.append(buffer, 0, take);
    buffer.erase(0, take);
    // remove the length left to read
    length -= take;

    char temp[512];

    // 2. Read remaining bytes from socket
    while (length > 0) {
        ssize_t bytes = recv(fd, temp, sizeof(temp), 0);
        if (bytes <= 0) return "";

        size_t used = std::min((size_t)bytes, length);
        result.append(temp, used);
        length -= used;

        // 3. Save overflow back into buffer
        if ((size_t)bytes > used) {
            buffer.append(temp + used, bytes - used);
        }
    }
    return result;
}


void handle_get(int client_fd, const std::string& key) {
    
    std::string reply;

    
    // Use a block {} to limit the scope of the lock
    {
        std::lock_guard<std::mutex> lock(store_mutex);
        auto it = store.find(key);
        if (it != store.end()) {
            reply = it->second + "\n";
        } else {
            reply = "NOT_FOUND\n";
        }
    } // Lock is released here automatically

    // Now send the data WITHOUT holding the lock
    send(client_fd, reply.c_str(), reply.size(), 0);
}

void process_command(int client_fd, const std::string& line, std::string& buffer) {
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;
    

    if (cmd == "PUT") {
        std::string key;
        size_t length;
        iss >> key >> length;
        std::cout << "put cmd" << std::endl;
        if (key.empty()) {
            std::string err = "ERROR invalid PUT\n";
            send(client_fd, err.c_str(), err.size(), 0);
            return;
        }
        std::string value = read_exactly(client_fd, length, buffer);
        if (value.empty() && length > 0) {
            std::cout << "empty read exact" << std::endl;
        }
        handle_put(client_fd, key, value);
    }
    else if (cmd == "GET") {
        std::string key;
        iss >> key;

        if (key.empty()) {
            std::string err = "ERROR invalid PUT\n";
            send(client_fd, err.c_str(), err.size(), 0);
            return;
        }
        handle_get(client_fd, key);
    }
    else {
        std::string err = "ERROR unknown command\n";
        send(client_fd, err.c_str(), err.size(), 0);
    }
}
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
    std::cout << "Buffer currently contains: [" << buffer << "]" << std::endl;
    size_t pos;
    while ((pos = buffer.find('\n')) != std::string::npos) {
        std::string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);

        process_command(new_socket, line, buffer);

    }
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
