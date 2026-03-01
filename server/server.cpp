#include "kv_store.h"
#include "logger.h"
#include "thread_pool.h"
#include "wal.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

// ── globals ───────────────────────────────────────────────────────────────────

KVStore                        g_store;
std::unique_ptr<WriteAheadLog> g_wal;

// Sequence numbers — every replicated write on the primary gets a unique,
// monotonically increasing number so the replica can detect ordering gaps.
std::atomic<uint64_t> g_write_seq{0};        // primary: incremented on every write
std::atomic<uint64_t> g_expected_seq{0};     // replica: last applied seq number
std::atomic<int64_t>  g_last_heartbeat_ms{0};// replica: steady-clock ms of last heartbeat

// Serialises all sends to replica_fd across client threads + heartbeat thread.
std::mutex g_replica_mutex;

constexpr int    PRIMARY_PORT         = 8080;
constexpr int    REPLICA_PORT         = 9090;
constexpr size_t THREAD_POOL_SIZE     = 8;
constexpr int    HEARTBEAT_INTERVAL_S = 2;
constexpr int    HEARTBEAT_TIMEOUT_S  = 6;  // promote after N seconds of silence

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
        close(fd); return -1;
    }
    return fd;
}

// ── command handlers ──────────────────────────────────────────────────────────

void handle_put(int client_fd, const std::string& key, const std::string& value,
                Role role, int replica_fd = -1) {
    if (g_wal) g_wal->log_put(key, value);
    g_store.put(key, value);
    Logger::info("PUT  key=" + key + "  bytes=" + std::to_string(value.size()));

    // Seq assignment and TCP send must be atomic under the same lock so that
    // the replica always sees RPUT messages in seq order even when multiple
    // client threads call handle_put concurrently.
    if (role == Role::PRIMARY && replica_fd >= 0) {
        std::lock_guard<std::mutex> lock(g_replica_mutex);
        uint64_t seq = ++g_write_seq;
        std::string msg = "RPUT " + std::to_string(seq) + " " + key +
                          " " + std::to_string(value.size()) + "\n" + value;
        if (send(replica_fd, msg.data(), msg.size(), 0) < 0)
            Logger::warn("Replication failed for key: " + key);
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
        std::lock_guard<std::mutex> lock(g_replica_mutex);
        uint64_t seq = ++g_write_seq;
        std::string cmd = "RDEL " + std::to_string(seq) + " " + key + "\n";
        send(replica_fd, cmd.data(), cmd.size(), 0);
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
        << " write_seq="  << g_write_seq.load()
        << "\n";
    send_response(client_fd, oss.str());
}

// ── protocol dispatcher ───────────────────────────────────────────────────────
//
// Client-facing:  PUT / GET / DEL / STATS
// Replication:    RPUT <seq> / RDEL <seq> / HEARTBEAT <seq>   (primary → replica only)

void process_command(int client_fd, const std::string& line,
                     std::string& buffer, Role role, int replica_fd = -1) {
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;

    // ── client-facing commands ────────────────────────────────────────────────
    if (cmd == "PUT") {
        if (role == Role::REPLICA) {
            send_response(client_fd, "ERROR replica is read-only\n"); return;
        }
        std::string key; size_t length = 0;
        iss >> key >> length;
        if (key.empty()) { send_response(client_fd, "ERROR invalid PUT\n"); return; }
        handle_put(client_fd, key, read_exactly(client_fd, length, buffer), role, replica_fd);

    } else if (cmd == "GET") {
        if (role == Role::REPLICA) {
            send_response(client_fd, "ERROR replica does not serve GET\n"); return;
        }
        std::string key; iss >> key;
        if (key.empty()) { send_response(client_fd, "ERROR invalid GET\n"); return; }
        handle_get(client_fd, key);

    } else if (cmd == "DEL") {
        if (role == Role::REPLICA) {
            send_response(client_fd, "ERROR replica is read-only\n"); return;
        }
        std::string key; iss >> key;
        if (key.empty()) { send_response(client_fd, "ERROR invalid DEL\n"); return; }
        handle_delete(client_fd, key, role, replica_fd);

    } else if (cmd == "STATS") {
        handle_stats(client_fd);

    // ── replication channel (primary → replica) ───────────────────────────────
    } else if (cmd == "RPUT") {
        uint64_t seq = 0; std::string key; size_t length = 0;
        iss >> seq >> key >> length;

        if (seq != g_expected_seq + 1)
            Logger::warn("[REPLICA] Seq gap: expected=" + std::to_string(g_expected_seq + 1) +
                         "  got=" + std::to_string(seq));
        g_expected_seq = seq;

        std::string value = read_exactly(client_fd, length, buffer);
        if (g_wal) g_wal->log_put(key, value);
        g_store.restore(key, value);
        Logger::info("[REPLICA] RPUT  seq=" + std::to_string(seq) + "  key=" + key);

    } else if (cmd == "RDEL") {
        uint64_t seq = 0; std::string key;
        iss >> seq >> key;

        if (seq != g_expected_seq + 1)
            Logger::warn("[REPLICA] Seq gap: expected=" + std::to_string(g_expected_seq + 1) +
                         "  got=" + std::to_string(seq));
        g_expected_seq = seq;

        if (g_wal) g_wal->log_delete(key);
        g_store.restore_remove(key);
        Logger::info("[REPLICA] RDEL  seq=" + std::to_string(seq) + "  key=" + key);

    } else if (cmd == "HEARTBEAT") {
        uint64_t primary_seq = 0;
        iss >> primary_seq;
        g_last_heartbeat_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        if (primary_seq > g_expected_seq)
            Logger::warn("[REPLICA] Lagging by " +
                         std::to_string(primary_seq - g_expected_seq) + " ops");

    } else if (!cmd.empty()) {
        send_response(client_fd, "ERROR unknown command\n");
    }
}

// ── per-connection receive loop ───────────────────────────────────────────────

enum class ClientStatus { OK, CLOSED, TIMEOUT };

// Returns OK on data, CLOSED on disconnect/error, TIMEOUT on SO_RCVTIMEO expiry.
// NOTE: the caller is responsible for closing the fd.
ClientStatus recv_from_client(int fd, std::string& buffer,
                               Role role, int primary_fd = -1) {
    char tmp[1024];
    ssize_t n = recv(fd, tmp, sizeof(tmp), 0);

    if (n == 0) {
        Logger::info("Connection closed by peer");
        return ClientStatus::CLOSED;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return ClientStatus::TIMEOUT;
        Logger::error("recv: " + std::string(strerror(errno)));
        return ClientStatus::CLOSED;
    }

    buffer.append(tmp, n);
    size_t pos;
    while ((pos = buffer.find('\n')) != std::string::npos) {
        std::string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);
        process_command(fd, line, buffer, role, primary_fd);
    }
    return ClientStatus::OK;
}

// Thin wrapper for the primary's thread pool — no recv timeout is set on client
// sockets so TIMEOUT never occurs; treat it as OK to keep the loop alive.
bool handle_client(int fd, std::string& buffer, Role role, int primary_fd = -1) {
    return recv_from_client(fd, buffer, role, primary_fd) != ClientStatus::CLOSED;
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
        if (replica_fd < 0) {
            Logger::warn("[PRIMARY] Replica unreachable — running without replication");
        } else {
            Logger::info("[PRIMARY] Connected to replica on :" + std::to_string(REPLICA_PORT));

            // Heartbeat thread: sends "HEARTBEAT <write_seq>\n" to the replica
            // every HEARTBEAT_INTERVAL_S seconds so the replica can detect a
            // frozen-but-connected primary before the TCP layer times out.
            std::thread([replica_fd] {
                while (true) {
                    std::this_thread::sleep_for(std::chrono::seconds(HEARTBEAT_INTERVAL_S));
                    std::string hb = "HEARTBEAT " + std::to_string(g_write_seq.load()) + "\n";
                    std::lock_guard<std::mutex> lock(g_replica_mutex);
                    if (send(replica_fd, hb.c_str(), hb.size(), 0) < 0) return;
                }
            }).detach();
        }
    }

    int server_fd = create_server_socket(port);
    Logger::info("[PRIMARY] Listening on :" + std::to_string(port));

    ThreadPool  pool(THREAD_POOL_SIZE);
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

    // Set a recv timeout so the loop wakes up periodically and can detect a
    // frozen primary (alive TCP, but no heartbeats for HEARTBEAT_TIMEOUT_S).
    timeval tv{};
    tv.tv_sec  = HEARTBEAT_INTERVAL_S;
    tv.tv_usec = 0;
    setsockopt(primary_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Seed timestamp so we don't fire immediately on startup.
    g_last_heartbeat_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    std::string buffer;
    while (true) {
        auto status = recv_from_client(primary_fd, buffer, Role::REPLICA, primary_fd);

        if (status == ClientStatus::CLOSED) {
            Logger::info("[REPLICA] Primary connection closed");
            break;
        }
        if (status == ClientStatus::TIMEOUT) {
            int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            int64_t elapsed_s = (now_ms - g_last_heartbeat_ms.load()) / 1000;
            if (elapsed_s >= HEARTBEAT_TIMEOUT_S) {
                Logger::warn("[REPLICA] No heartbeat for " + std::to_string(elapsed_s) +
                             "s — promoting to PRIMARY");
                break;
            }
        }
    }

    close(primary_fd);
    close(server_fd);

    Logger::info("[REPLICA] Promoting to PRIMARY on :" + std::to_string(PRIMARY_PORT));
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

    if (role == Role::PRIMARY) run_primary_server(port);
    else                       run_replica_server(port);
}
