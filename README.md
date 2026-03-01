# keyvalStore

A distributed key-value store written in C++17 with primary-replica replication, automatic failover, crash recovery, heartbeat-based failure detection, and sequence-numbered replication.

## Features

- **Primary-replica replication** — every write is synchronously forwarded to the replica with a monotonic sequence number
- **Heartbeat-based failover** — primary sends heartbeats every 2 s; replica promotes itself after 6 s of silence, catching frozen-but-connected primaries that TCP alone would miss
- **Client reconnect** — client transparently retries the replica when the primary is unreachable
- **Write-Ahead Log (WAL)** — all mutations are persisted to disk; state is fully restored on restart
- **Full CRUD** — `PUT`, `GET`, `DEL`, and `STATS` commands
- **Thread pool** — fixed-size pool of 8 worker threads handles concurrent client connections
- **Structured logging** — timestamped `INFO / WARN / ERROR` output on the server
- **Google Test suite** — 25 unit tests covering the store, WAL, and thread pool
- **Benchmark tool** — measures PUT/GET throughput and P50/P95/P99 latency

## Architecture

```
┌─────────┐       ┌──────────────────────────────┐       ┌──────────────────┐
│  Client │──────▶│  Primary :8080               │──────▶│  Replica  :9090  │
└─────────┘       │  RPUT <seq> / RDEL <seq>     │       │  replica.wal     │
                  │  HEARTBEAT <seq> every 2s    │       │                  │
                  │  primary.wal                 │       │                  │
                  └──────────────────────────────┘       └──────────────────┘
```

When the primary goes down (TCP drop **or** heartbeat timeout):

```
┌─────────┐                               ┌──────────────────────────┐
│  Client │──────────────────────────────▶│  Promoted replica :8080  │
└─────────┘    (auto-reconnect)           │  primary.wal (new)       │
                                          └──────────────────────────┘
```

## Project Structure

```
keyvalStore/
├── Makefile
├── server/
│   ├── server.cpp        # Entry point, server runners, protocol dispatcher
│   ├── kv_store.h        # Thread-safe KVStore class with atomic stats
│   ├── wal.h             # Write-Ahead Log for crash recovery (mutex-protected)
│   ├── logger.h          # Timestamped structured logger
│   └── thread_pool.h     # Fixed-size thread pool with condition variable queue
├── client/
│   └── client.cpp        # Interactive CLI client with automatic failover reconnect
├── tests/
│   ├── CMakeLists.txt    # Fetches Google Test via FetchContent
│   ├── test_kv_store.cpp # 12 tests: CRUD, binary values, stats, concurrency
│   ├── test_wal.cpp      # 7 tests: replay, binary encoding, interleaved ops
│   └── test_thread_pool.cpp # 6 tests: correctness, concurrency, concurrent submit
└── bench/
    └── bench.cpp         # PUT/GET throughput and latency benchmark
```

## Build

Requires a C++17-capable compiler (`g++` or `clang++`) and POSIX sockets (Linux or macOS).

```bash
make            # builds ./kvserver and ./kvclient
make kvbench    # builds the benchmark tool
make test       # downloads Google Test, builds, and runs all 25 tests (requires cmake)
make clean      # removes binaries, *.wal files, and the test build directory
```

## Usage

**Start the replica first** (it waits for the primary to connect):

```bash
./kvserver replica 9090
```

**Start the primary:**

```bash
./kvserver primary 8080
```

**Run the client:**

```bash
./kvclient
```

### Commands

| Command | Description | Example |
|---|---|---|
| `PUT <key>` | Store a value (prompts for value separately) | `PUT username` |
| `GET <key>` | Retrieve a value | `GET username` |
| `DEL <key>` | Delete a key | `DEL username` |
| `STATS` | Show live server statistics | `STATS` |

```
Commands: PUT <key>  GET <key>  DEL <key>  STATS
          (Ctrl+D to exit)

> PUT name
Value: alice
OK
> GET name
alice
> STATS
puts=1 gets=1 deletes=0 connections=1 write_seq=1
> DEL name
OK
> GET name
NOT_FOUND
```

## Failover Demo

### TCP disconnect (primary process killed)

1. Start replica, then primary, then client
2. `PUT` a few keys
3. Kill the primary (`Ctrl+C`)
4. Replica logs: `[REPLICA] Primary connection closed` → `Promoting to PRIMARY on :8080`
5. Client logs: `Server disconnected. Attempting failover...` then reconnects
6. `GET` the same keys — all data is preserved

### Heartbeat timeout (frozen primary)

1. Same setup as above
2. Pause the primary instead of killing it: `kill -STOP <pid>`
3. After 6 s the replica logs: `[REPLICA] No heartbeat for 6s — promoting to PRIMARY`
4. Client reconnects to the promoted replica transparently

## Persistence Demo

1. Start the primary and `PUT` some keys
2. Kill the primary (`Ctrl+C`)
3. Restart: `./kvserver primary 8080`
4. All keys are immediately available again — restored from `primary.wal`

## Benchmark

```bash
make kvbench
./kvserver primary 8080   # in another terminal
./kvbench --ops 10000 --value-size 64
```

Example output:
```
Connected to 127.0.0.1:8080  ops=10000  value_size=64 bytes

── PUT (10000 ops, 64-byte values) ──────────────────────────────
  throughput : 48203 ops/sec
  avg latency:   20 µs   p50:   17 µs   p95:   42 µs   p99:   88 µs

── GET (10000 ops, 64-byte values) ──────────────────────────────
  throughput : 61540 ops/sec
  avg latency:   16 µs   p50:   13 µs   p95:   33 µs   p99:   71 µs
```

## Tests

```bash
make test
```

```
[==========] Running 25 tests from 3 test suites.
[  PASSED  ] 25 tests.
```

| Suite | Count | Covers |
|---|---|---|
| `KVStoreTest` | 12 | CRUD, binary values, WAL-replay helpers, stats, connection tracking, thread safety |
| `WALTest` | 7 | PUT/DEL replay, empty log, embedded newlines, null bytes, 100-entry log, interleaved ops |
| `ThreadPoolTest` | 6 | Task execution, large batch, sum correctness, concurrent execution, single thread, concurrent submit |

## Technical Details

### Replication Protocol

Two separate wire formats share the same TCP connection between primary and replica:

**Client → Primary:**
```
PUT <key> <value_bytes>\n<value>    # length-prefixed, binary-safe
GET <key>\n
DEL <key>\n
STATS\n
```

**Primary → Replica (replication channel):**
```
RPUT <seq> <key> <value_bytes>\n<value>    # seq ensures ordering
RDEL <seq> <key>\n
HEARTBEAT <seq>\n                          # sent every 2 s
```

Sequence number assignment and the TCP send to the replica happen **atomically under the same mutex**, guaranteeing in-order delivery even when multiple client threads write concurrently.

### Failover Mechanism

| Failure type | Detection | Latency |
|---|---|---|
| Primary process killed / network drop | TCP EOF (`recv` returns 0) | < 1 s |
| Primary frozen / deadlocked | Heartbeat timeout | ≤ 6 s (configurable) |

### Write-Ahead Log Format

```
PUT username 5
alice
DEL username
```

Values are length-prefixed, making the format binary-safe (values may contain newlines or null bytes). The WAL is mutex-protected, allowing concurrent client threads to append safely.

### Concurrency Model

- Primary uses a fixed `ThreadPool` of 8 worker threads — one task per client connection
- `KVStore` protects all data access with `std::mutex`
- Stats counters use `std::atomic<uint64_t>` for lock-free reads from any thread
- All sends to the replica (client threads + heartbeat thread) are serialised through `g_replica_mutex`; seq increment happens inside the same lock to guarantee ordering
