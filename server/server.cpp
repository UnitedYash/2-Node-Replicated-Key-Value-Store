#include "kv_store.h"
#include "logger.h"
#include "thread_pool.h"
#include "wal.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

// ── globals ───────────────────────────────────────────────────────────────────

KVStore                        g_store;
std::unique_ptr<WriteAheadLog> g_wal;

constexpr int    PRIMARY_PORT     = 8080;
constexpr int    REPLICA_PORT     = 9090;
constexpr size_t THREAD_POOL_SIZE = 8;

enum class Role { PRIMARY, REPLICA };

// ── low-level helpers ─────────────────────────────────────────────────────────

void send_response(int fd, std::string_view msg) {
    send(fd, msg.data(), msg.size(), 0);
}

// Reads exactly `length` bytes from `fd`, using `buffer` as an overflow cache.
[[nodiscard]] std::string read_exactly(int fd, size_t length, std::string& buffer) {
    std::string result;

    size_t take = std::min(length, buffer.size());
    result.append(buffer, 0, take);
    buffer.erase(0, take);
    length -= take;

    char tmp[512];
    while (length > 0) {
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return {};

        size_t used = std::min(static_cast<size_t>(n), length);
        result.append(tmp, used);
        length -= used;

        if (static_cast<size_t>(n) > used)
            buffer.append(tmp + used, n - used);
    }
    return result;
}

// Creates, binds, and begins listening on `port`.  Exits on failure.
[[nodiscard]] int create_server_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(EXIT_FAILURE); }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind"); exit(EXIT_FAILURE);
    }
    if (listen(fd, SOMAXCONN) < 0) {
        perror("listen"); exit(EXIT_FAILURE);
    }
    return fd;
}

[[nodiscard]] int connect_to_replica(const std::string& host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// ── command handlers ──────────────────────────────────────────────────────────

void handle_put(int client_fd, const std::string& key, const std::string& value,
                Role role, int replica_fd = -1) {
    if (g_wal) g_wal->log_put(key, value);
    g_store.put(key, value);
    Logger::info("PUT  key=" + key + "  bytes=" + std::to_string(value.size()));

    if (role == Role::PRIMARY && replica_fd >= 0) {
        std::ostringstream oss;
        oss << "PUT " << key << " " << value.size() << "\n";
        std::string header = oss.str();
        send(replica_fd, header.c_str(), header.size(), 0);
        send(replica_fd, value.data(),   value.size(),  0);
    }

    if (role == Role::PRIMARY) send_response(client_fd, "OK\n");
}

void handle_get(int client_fd, const std::string& key) {
    Logger::info("GET  key=" + key);
    if (auto val = g_store.get(key)) {
        std::string reply = *val + "\n";
        send_response(client_fd, reply);
    } else {
        send_response(client_fd, "NOT_FOUND\n");
    }
}

void handle_delete(int client_fd, const std::string& key,
                   Role role, int replica_fd = -1) {
    if (g_wal) g_wal->log_delete(key);
    bool found = g_store.remove(key);
    Logger::info("DEL  key=" + key + (found ? "  (removed)" : "  (not found)"));

    if (role == Role::PRIMARY && replica_fd >= 0) {
        std::string cmd = "DEL " + key + "\n";
        send(replica_fd, cmd.c_str(), cmd.size(), 0);
    }

    if (role == Role::PRIMARY)
        send_response(client_fd, found ? "OK\n" : "NOT_FOUND\n");
}

void handle_stats(int client_fd) {
    const auto& s = g_store.stats();
    std::ostringstream oss;
    oss << "puts="        << s.total_puts.load()
        << " gets="       << s.total_gets.load()
        << " deletes="    << s.total_deletes.load()
        << " connections="<< s.active_connections.load()
        << "\n";
    send_response(client_fd, oss.str());
}

// ── protocol dispatcher ───────────────────────────────────────────────────────

void process_command(int client_fd, const std::string& line,
                     std::string& buffer, Role role, int replica_fd = -1) {
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;

    if (cmd == "PUT") {
        if (role == Role::REPLICA && client_fd != replica_fd) {
            send_response(client_fd, "ERROR replica is read-only\n");
            return;
        }
        std::string key;
        size_t length = 0;
        iss >> key >> length;
        if (key.empty()) { send_response(client_fd, "ERROR invalid PUT\n"); return; }

        std::string value = read_exactly(client_fd, length, buffer);
        if (value.empty() && length > 0)
            Logger::warn("Empty value read for key: " + key);

        handle_put(client_fd, key, value, role, replica_fd);

    } else if (cmd == "GET") {
        if (role == Role::REPLICA) {
            send_response(client_fd, "ERROR replica does not serve GET\n");
            return;
        }
        std::string key;
        iss >> key;
        if (key.empty()) { send_response(client_fd, "ERROR invalid GET\n"); return; }

        handle_get(client_fd, key);

    } else if (cmd == "DEL") {
        if (role == Role::REPLICA && client_fd != replica_fd) {
            send_response(client_fd, "ERROR replica is read-only\n");
            return;
        }
        std::string key;
        iss >> key;
        if (key.empty()) { send_response(client_fd, "ERROR invalid DEL\n"); return; }

        handle_delete(client_fd, key, role, replica_fd);

    } else if (cmd == "STATS") {
        handle_stats(client_fd);

    } else if (!cmd.empty()) {
        send_response(client_fd, "ERROR unknown command\n");
    }
}

// ── per-connection loop ───────────────────────────────────────────────────────

// Reads one batch of data from `fd`, dispatches complete newline-delimited
// commands, and returns false when the connection should be closed.
// NOTE: the caller is responsible for closing `fd`.
bool handle_client(int fd, std::string& buffer, Role role, int primary_fd = -1) {
    char tmp[1024];
    ssize_t n = recv(fd, tmp, sizeof(tmp), 0);

    if (n <= 0) {
        if (n == 0) Logger::info("Connection closed by peer");
        else        Logger::error("recv: " + std::string(strerror(errno)));
        return false;
    }

    buffer.append(tmp, n);

    size_t pos;
    while ((pos = buffer.find('\n')) != std::string::npos) {
        std::string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);
        process_command(fd, line, buffer, role, primary_fd);
    }
    return true;
}

// ── server runners ────────────────────────────────────────────────────────────

void run_primary_server(int port, bool with_replica = true) {
    g_wal = std::make_unique<WriteAheadLog>("primary.wal");
    g_wal->replay(
        [](const std::string& k, const std::string& v) { g_store.restore(k, v); },
        [](const std::string& k)                        { g_store.restore_remove(k); }
    );
    Logger::info("[PRIMARY] WAL replay complete");

    int replica_fd = -1;
    if (with_replica) {
        replica_fd = connect_to_replica("127.0.0.1", REPLICA_PORT);
        if (replica_fd < 0)
            Logger::warn("[PRIMARY] Replica unreachable — running without replication");
        else
            Logger::info("[PRIMARY] Connected to replica on :" + std::to_string(REPLICA_PORT));
    }

    int server_fd = create_server_socket(port);
    Logger::info("[PRIMARY] Listening on :" + std::to_string(port));

    ThreadPool pool(THREAD_POOL_SIZE);
    sockaddr_in addr{};
    socklen_t   addrlen = sizeof(addr);

    while (true) {
        int client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&addr), &addrlen);
        if (client_fd < 0) { perror("accept"); continue; }

        Logger::info("[PRIMARY] Client connected");
        g_store.on_connect();

        pool.submit([client_fd, replica_fd] {
            std::string buf;
            while (handle_client(client_fd, buf, Role::PRIMARY, replica_fd)) {}
            close(client_fd);
            g_store.on_disconnect();
            Logger::info("[PRIMARY] Client disconnected");
        });
    }
}

void run_replica_server(int port) {
    g_wal = std::make_unique<WriteAheadLog>("replica.wal");
    g_wal->replay(
        [](const std::string& k, const std::string& v) { g_store.restore(k, v); },
        [](const std::string& k)                        { g_store.restore_remove(k); }
    );
    Logger::info("[REPLICA] WAL replay complete");

    int server_fd = create_server_socket(port);
    Logger::info("[REPLICA] Listening on :" + std::to_string(port));

    sockaddr_in addr{};
    socklen_t   addrlen = sizeof(addr);
    int primary_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&addr), &addrlen);
    if (primary_fd < 0) { perror("accept"); exit(EXIT_FAILURE); }
    Logger::info("[REPLICA] Primary connected");

    std::string buffer;
    while (handle_client(primary_fd, buffer, Role::REPLICA, primary_fd)) {}

    close(primary_fd);
    close(server_fd);

    Logger::info("[REPLICA] Primary disconnected — promoting to PRIMARY on :" +
                 std::to_string(PRIMARY_PORT));
    run_primary_server(PRIMARY_PORT, /*with_replica=*/false);
}

// ── entry point ───────────────────────────────────────────────────────────────

[[nodiscard]] Role parse_role(std::string_view arg) {
    if (arg == "primary") return Role::PRIMARY;
    if (arg == "replica") return Role::REPLICA;
    std::cerr << "Invalid role '" << arg << "'. Use 'primary' or 'replica'.\n";
    exit(EXIT_FAILURE);
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <primary|replica> <port>\n";
        return EXIT_FAILURE;
    }

    Role role = parse_role(argv[1]);
    int  port = std::stoi(argv[2]);

    if (role == Role::PRIMARY) {
        run_primary_server(port);
    } else {
        run_replica_server(port);
    }
}
