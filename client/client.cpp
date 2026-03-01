#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

constexpr int PRIMARY_PORT = 8080;
constexpr int REPLICA_PORT = 9090;
constexpr int BUFFER_SIZE  = 4096;

// ── connection helpers ────────────────────────────────────────────────────────

[[nodiscard]] int try_connect(const char* host, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        close(sock); return -1;
    }
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(sock); return -1;
    }
    return sock;
}

// Tries primary first, falls back to replica (which may have been promoted).
[[nodiscard]] int connect_with_fallback() {
    if (int sock = try_connect("127.0.0.1", PRIMARY_PORT); sock >= 0) {
        std::cout << "Connected to primary (:" << PRIMARY_PORT << ")\n";
        return sock;
    }
    std::cout << "Primary unreachable — trying replica (:" << REPLICA_PORT << ")...\n";
    if (int sock = try_connect("127.0.0.1", REPLICA_PORT); sock >= 0) {
        std::cout << "Connected to replica (:" << REPLICA_PORT << ")\n";
        return sock;
    }
    return -1;
}

// ── message builder ───────────────────────────────────────────────────────────

// Parses user input and returns the wire-format message to send.
// Returns an empty string if the input is invalid or incomplete.
[[nodiscard]] std::string build_message(const std::string& input) {
    std::istringstream iss(input);
    std::string cmd;
    iss >> cmd;

    if (cmd == "PUT") {
        std::string key;
        iss >> key;
        if (key.empty()) {
            std::cerr << "Usage: PUT <key>\n";
            return {};
        }
        std::string value;
        std::cout << "Value: ";
        if (!std::getline(std::cin, value)) return {};
        // Wire format: "PUT <key> <len>\n<value>"
        return "PUT " + key + " " + std::to_string(value.size()) + "\n" + value;

    } else if (cmd == "GET" || cmd == "DEL" || cmd == "STATS") {
        return input + "\n";

    }

    std::cerr << "Unknown command. Supported: PUT <key>  GET <key>  DEL <key>  STATS\n";
    return {};
}

// ── main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "Commands: PUT <key>  GET <key>  DEL <key>  STATS\n"
              << "          (Ctrl+D to exit)\n\n";

    char buffer[BUFFER_SIZE] = {};

    while (true) {
        int sock = connect_with_fallback();
        if (sock < 0) {
            std::cerr << "No server available. Retrying in 2s...\n";
            sleep(2);
            continue;
        }

        while (true) {
            std::string input;
            std::cout << "> ";
            if (!std::getline(std::cin, input)) {
                std::cout << "EOF — exiting.\n";
                close(sock);
                return 0;
            }
            if (input.empty()) continue;

            std::string msg = build_message(input);
            if (msg.empty()) continue;

            if (send(sock, msg.c_str(), msg.size(), 0) < 0) {
                std::cout << "Send failed. Attempting failover...\n";
                close(sock); break;
            }

            memset(buffer, 0, BUFFER_SIZE);
            ssize_t n = read(sock, buffer, BUFFER_SIZE - 1);
            if (n <= 0) {
                std::cout << "Server disconnected. Attempting failover...\n";
                close(sock); break;
            }

            std::cout << buffer;
        }
    }
}
