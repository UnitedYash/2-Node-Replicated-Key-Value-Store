// bench/bench.cpp — throughput and latency benchmark for keyvalStore
//
// Usage: ./kvbench [--ops N] [--port PORT] [--value-size BYTES]
//
// Requires a running kvserver. Measures sequential PUT and GET
// throughput (ops/sec) and reports P50/P95/P99 latency.
//
// Example output:
//   ── PUT (10000 ops, 64-byte values) ─────────────────────────
//     throughput : 45231 ops/sec
//     avg latency:   22 µs   p50:   18 µs   p95:   47 µs   p99:   91 µs

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <numeric>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

// ── connection ────────────────────────────────────────────────────────────────

static int g_sock = -1;

static bool connect_server(const char* host, int port) {
    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);
    return connect(g_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
}

// ── wire protocol helpers ─────────────────────────────────────────────────────

static char g_recv[65536];

static bool send_recv(const std::string& msg) {
    if (send(g_sock, msg.data(), msg.size(), 0) < 0) return false;
    ssize_t n = recv(g_sock, g_recv, sizeof(g_recv) - 1, 0);
    return n > 0;
}

static std::string make_put(const std::string& key, const std::string& value) {
    return "PUT " + key + " " + std::to_string(value.size()) + "\n" + value;
}

static std::string make_get(const std::string& key) {
    return "GET " + key + "\n";
}

// ── stats reporting ───────────────────────────────────────────────────────────

static void print_stats(const std::string& label,
                        size_t value_bytes,
                        const std::vector<double>& latencies_us,
                        double elapsed_s) {
    if (latencies_us.empty()) return;

    std::vector<double> s = latencies_us;
    std::sort(s.begin(), s.end());

    auto pct = [&](int p) { return s[s.size() * static_cast<size_t>(p) / 100]; };
    double avg        = std::accumulate(s.begin(), s.end(), 0.0) / s.size();
    double throughput = s.size() / elapsed_s;

    std::cout << "\n── " << label
              << " (" << s.size() << " ops, " << value_bytes << "-byte values)"
              << " ──────────────────────────────\n"
              << "  throughput : " << static_cast<int>(throughput) << " ops/sec\n"
              << "  avg latency: " << static_cast<int>(avg)     << " µs"
              << "   p50: "        << static_cast<int>(pct(50)) << " µs"
              << "   p95: "        << static_cast<int>(pct(95)) << " µs"
              << "   p99: "        << static_cast<int>(pct(99)) << " µs\n";
}

// ── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    int    ops        = 10000;
    int    port       = 8080;
    size_t value_size = 64;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--ops"        && i + 1 < argc) ops        = std::stoi(argv[++i]);
        if (arg == "--port"       && i + 1 < argc) port       = std::stoi(argv[++i]);
        if (arg == "--value-size" && i + 1 < argc) value_size = std::stoul(argv[++i]);
    }

    if (!connect_server("127.0.0.1", port)) {
        std::cerr << "Failed to connect to 127.0.0.1:" << port
                  << " — is kvserver running?\n";
        return 1;
    }
    std::cout << "Connected to 127.0.0.1:" << port
              << "  ops=" << ops << "  value_size=" << value_size << " bytes\n";

    const std::string value(value_size, 'x');

    // ── PUT benchmark ─────────────────────────────────────────────────────────
    std::vector<double> put_lat;
    put_lat.reserve(ops);
    auto put_start = std::chrono::steady_clock::now();

    for (int i = 0; i < ops; ++i) {
        std::string msg = make_put("bench:" + std::to_string(i), value);
        auto t0 = std::chrono::steady_clock::now();
        if (!send_recv(msg)) { std::cerr << "PUT failed at op " << i << "\n"; break; }
        put_lat.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count());
    }
    double put_elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - put_start).count();

    // ── GET benchmark ─────────────────────────────────────────────────────────
    std::vector<double> get_lat;
    get_lat.reserve(ops);
    auto get_start = std::chrono::steady_clock::now();

    for (int i = 0; i < ops; ++i) {
        std::string msg = make_get("bench:" + std::to_string(i));
        auto t0 = std::chrono::steady_clock::now();
        if (!send_recv(msg)) { std::cerr << "GET failed at op " << i << "\n"; break; }
        get_lat.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count());
    }
    double get_elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - get_start).count();

    // ── results ───────────────────────────────────────────────────────────────
    print_stats("PUT", value_size, put_lat, put_elapsed);
    print_stats("GET", value_size, get_lat, get_elapsed);
    std::cout << "\n";

    close(g_sock);
    return 0;
}
