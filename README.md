# keyvalStore

A distributed key-value store written in C++17 with primary-replica replication, automatic failover, and crash recovery via a Write-Ahead Log.

## Features

- **Primary-replica replication** — every write is synchronously forwarded to the replica
- **Automatic failover** — replica detects a dropped primary and promotes itself, taking over the primary's port
- **Client reconnect** — client transparently retries the replica when the primary is unreachable
- **Write-Ahead Log (WAL)** — all mutations are persisted to disk; state is fully restored on restart
- **Full CRUD** — `PUT`, `GET`, `DEL`, and `STATS` commands
- **Thread pool** — fixed-size pool of 8 worker threads handles concurrent client connections
- **Structured logging** — timestamped `INFO / WARN / ERROR` output on the server

## Architecture

```
┌─────────┐       ┌─────────────────┐       ┌──────────────────┐
│  Client │──────▶│  Primary :8080  │──────▶│  Replica  :9090  │
└─────────┘       │  primary.wal    │       │  replica.wal     │
                  └─────────────────┘       └──────────────────┘
```

When the primary goes down:

```
┌─────────┐                               ┌──────────────────────────┐
│  Client │──────────────────────────────▶│  Promoted replica :8080  │
└─────────┘                               │  primary.wal (new)       │
                                          └──────────────────────────┘
```

The replica detects the TCP disconnect, promotes itself to primary on port `8080`, and begins accepting client connections with all previously replicated data intact.

## Project Structure

```
keyvalStore/
├── Makefile
├── server/
│   ├── server.cpp        # Entry point, server runners, protocol dispatcher
│   ├── kv_store.h        # Thread-safe KVStore class with atomic stats
│   ├── wal.h             # Write-Ahead Log for crash recovery
│   ├── logger.h          # Timestamped structured logger
│   └── thread_pool.h     # Fixed-size thread pool
└── client/
    └── client.cpp        # Interactive CLI client with failover reconnect
```

## Build

Requires a C++17-capable compiler and POSIX sockets (Linux or macOS).

```bash
make          # builds ./kvserver and ./kvclient
make clean    # removes binaries and *.wal files
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
| `PUT <key>` | Store a value (prompts for value) | `PUT username` |
| `GET <key>` | Retrieve a value | `GET username` |
| `DEL <key>` | Delete a key | `DEL username` |
| `STATS` | Show server statistics | `STATS` |

```
Commands: PUT <key>  GET <key>  DEL <key>  STATS
          (Ctrl+D to exit)

> PUT name
Value: alice
OK
> GET name
alice
> STATS
puts=1 gets=1 deletes=0 connections=1
> DEL name
OK
> GET name
NOT_FOUND
```

## Failover Demo

1. Start replica, then primary, then client
2. `PUT` a few keys via the client
3. Kill the primary (`Ctrl+C` in its terminal)
4. The replica logs: `[REPLICA] Primary disconnected — promoting to PRIMARY on :8080`
5. The client logs: `Server disconnected. Attempting failover...` then reconnects automatically
6. `GET` the same keys — all data is preserved from replication

## Persistence Demo

1. Start the primary and `PUT` some keys
2. Kill the primary (`Ctrl+C`)
3. Restart it: `./kvserver primary 8080`
4. All keys are immediately available again — restored from `primary.wal`

## Technical Details

### Replication Protocol

Commands are newline-delimited text over TCP:

```
PUT <key> <value_bytes>\n<value>    # length-prefixed for binary safety
GET <key>\n
DEL <key>\n
STATS\n
```

The replica only accepts `PUT` and `DEL` from the primary connection; it rejects client reads/writes until promoted.

### Write-Ahead Log Format

```
PUT username 5
alice
DEL username
```

On startup, the WAL is replayed top-to-bottom to restore the store before the server accepts any connections.

### Concurrency Model

- Primary spawns tasks into a fixed `ThreadPool` (8 threads) — one task per client connection
- All store access is protected by a `std::mutex` inside `KVStore`
- Stats counters use `std::atomic<uint64_t>` for lock-free reads
