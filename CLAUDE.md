# CLAUDE.md

C++17 prototype of a sharded spatial database for 3D points (mmWave radar returns): mmap'd WAL → buffer pool → Z-order (Morton) index, with a TCP worker per shard, a 2PC coordinator, primary→standby replication, failover, and shard splitting. POSIX only (macOS/Linux). Coursework, ~1500 lines, flat directory, no dependencies.

## Build and run

No build system. Compile every `.cpp` together:

```bash
g++ -std=c++17 -O2 -Wall -pthread -o spatial_db \
    main.cpp Coordinator.cpp NetworkClient.cpp WorkerServer.cpp \
    BufferPool.cpp SpatialIndex.cpp MortonCode.cpp WriteAheadLog.cpp \
    MortonClient.cpp QueryOptimizer.cpp KittiLoader.cpp
rm -f kitti_wal.dat   # for a clean run (see WAL notes)
./spatial_db          # or: ./spatial_db path/to/frame.bin (default 000000.bin)
```

There are no unit tests. [main.cpp](main.cpp) is now the KITTI demo: it starts one in-process worker on 8081 (which must be free), ingests a Velodyne `.bin` frame through `KittiLoader`, and runs a bounding-box query through the SDK. `000000.bin` is untracked, so it has to be present locally. A clean run of it ingests 115,384 / 115,384 points and the query returns 20,264 rows. The program exits by detaching the worker thread.

The demo's output is not a reliable check: it prints `[PASS]` even for 0 rows, and exits 0 after a failed query. Compare the ingested and returned counts instead.

### Cluster integration tests
TESTs 0-4 were removed from `main.cpp` by commit `6d4c44a`, so nothing in the working tree covers 2PC across shards, replication, failover or splitting. To run them after a change to those paths, compile `git show a20327a:main.cpp` in place of `main.cpp` and without `KittiLoader.cpp`, with ports 8081-8083 free and `primary_wal.dat standby_wal.dat shard2_wal.dat` deleted:
- TEST 0: a committing cross-shard 2PC (tx 9001) and an aborting one (tx 9002)
- TEST 1: ingest + replication to the standby
- TEST 2: stop 8081 and fail over to 8082, then query the standby
- TEST 3: split 8082's shard onto 8083
- TEST 4: the SDK and optimizer against the cluster left by TESTs 2 and 3

Each check prints a `>>> PASS` or `>>> FAIL` line, and any failure makes the program exit 1. That version's two abort cases (tx 9002 and the SDK insert) use `x = -1`, which is now valid. Change both to `5000.0f` or 3 of the 18 checks fail; with that change all 18 pass.

## Architecture

| File | Role |
|---|---|
| [SpatialRecord.h](SpatialRecord.h) | Packed 24-byte record `{uint64 timestamp; float x,y,z; uint32 metadata}`. It goes on the wire and into the WAL as raw bytes. |
| [WriteAheadLog.h](WriteAheadLog.h)/.cpp | Fixed-size `mmap` file: `WalHeader` (magic `"MORTONWA"`, version, entry_count) followed by an array of `LogEntry{type, tx_id, rec}`. `append_entry` is mutex-guarded and returns `false` when full. `for_each` iterates entries for recovery. |
| [BufferPool.h](BufferPool.h)/.cpp | Batches records and flushes them into `SpatialIndex` once `capacity` is reached. |
| [MortonCode.h](MortonCode.h)/.cpp | `quantize_coord`: `(v + 1000) * 1000`, clamped to 21 bits, monotone. `coord_in_range` is false for anything it would clamp, plus NaN and infinity. `calculate_z_order` interleaves x→bit0, y→bit1, z→bit2 into a 63-bit key. `point_key` goes straight from raw coordinates to a key. `MAX_MORTON_KEY` is the top shard bound. |
| [SpatialIndex.h](SpatialIndex.h)/.cpp | `std::map<uint64 zkey, SpatialRecord>`. `range_query` scans `[zkey(min corner), zkey(max corner)]` and filters on exact coordinates. `extract_and_remove_range` is used for migration. |
| [NodeConfig.h](NodeConfig.h) | `ShardRange{node_id, start_key, end_key}` (inclusive bounds). |
| [Coordinator.h](Coordinator.h)/.cpp | Shard map + `node_id → NodeEndpoint`. Methods: `route_query`, `execute_2pc_transaction`, `execute_query`, `split_shard`, `failover_primary_to_standby`. |
| [NetworkProtocol.h](NetworkProtocol.h) | `CommandType` enum and the packed payload structs. |
| [NetworkClient.h](NetworkClient.h)/.cpp | Blocking, non-copyable client with one socket and a 5 s `SO_RCVTIMEO`. A failed or partial reply counts as abort/NACK, and `send_query` returns false. |
| [MortonClient.h](MortonClient.h)/.cpp | The SDK. `MortonClient` wraps a `Coordinator&`: `insert` is a one-record 2PC, `insert_batch` is one all-or-nothing 2PC for many records, and `query()` builds a `SpatialQueryBuilder` whose `execute()` returns a `QueryResult{ok, records, error, plan}`. It holds no connection between calls. |
| [QueryOptimizer.h](QueryOptimizer.h)/.cpp | `optimize` returns an `ExecutionPlan`: one key range, up to 8 disjoint sub-boxes, or `INVALID_PLAN`. `SpatialQueryBuilder::execute` runs `plan.sub_queries`. |
| [KittiLoader.h](KittiLoader.h)/.cpp | Reads a raw KITTI `.bin` frame (4 floats per point: x, y, z, reflectance) and sends it through `insert_batch`, 1,000 points per transaction. Reflectance is stored in `metadata`, scaled to 0-255. Timestamps are 0. |
| [WorkerServer.h](WorkerServer.h)/.cpp | Per-shard server. `start_listening()` runs `recover()` first, then serves one connection at a time, dispatching on `header.command`. |

### Wire protocol
Each message is a packed `PacketHeader{uint8 command; uint32 payload_len}` followed by a fixed struct. Byte order is host-endian, so both ends must share an architecture. Workers read payloads by `sizeof(struct)` and ignore `payload_len`, except in `CMD_QUERY_RESP`. The worker replies to PREPARE with `CMD_VOTE_COMMIT`/`CMD_VOTE_ABORT`, and to COMMIT/ABORT/MIGRATE with `CMD_ACK`. A failed MIGRATE gets `CMD_ABORT` back as a NACK. `CMD_INGEST` and `CMD_REPLICATE` get no reply.

`CMD_HEARTBEAT` (0x0c) and `CMD_SPLIT_SHARD` (0x0d) / `SplitShardPayload` are declared but not yet handled anywhere (work in progress). A new command needs three things: an enum value plus a packed payload in `NetworkProtocol.h`, a `send_*` in `NetworkClient`, and a branch in the `WorkerServer::start_listening` dispatch chain.

### Key flows
- **Routing:** coordinate → `quantize_coord` → `calculate_z_order` → linear scan of `cluster_shards`. `node_id 0` means "no shard", so never register node 0. The demo uses node_id == port. A node needs both `register_shard` (its keys) and `register_node` (its address); without the address, 2PC aborts with `no shard/endpoint for point`, the same message as an unroutable key.
- **2PC** (`execute_2pc_transaction`): records are grouped by owning shard, and the whole tx is refused if any record is unroutable. Phase 1 sends one `CMD_PREPARE` per record, only to the owning shards. The worker votes abort if any coordinate fails `coord_in_range` (NaN, infinity, or outside the quantizer's range); otherwise it stages the record in `staged_tx[tx_id]`. Phase 2 commits on a unanimous vote. A missing ACK is retried once on a fresh connection; worker COMMIT is idempotent (an unknown tx_id is still ACKed). Otherwise every connected node gets ABORT.
- **SDK query:** `optimize` splits the box when that cuts the total key span at least 4x. Each sub-box goes to `execute_query`, which asks each overlapping shard once. If any shard fails, the query fails rather than returning partial rows.
- **Replication:** after a local `CMD_INGEST` or a committed record, `forward_to_standby` opens a new connection per record and sends `CMD_REPLICATE`. This is fire-and-forget, with no ack and no catch-up. The standby doesn't forward further.
- **Failover:** a purely coordinator-side remap of `node_id` in the shard map. It doesn't detect failures.
- **Split** (`split_shard`): halve the old node's key range, then send `CMD_MIGRATE_RANGE`. The old worker extracts the upper half and re-sends it to the target as `CMD_INGEST`. If the target is unreachable, the worker restores the records locally and NACKs. The coordinator changes the shard map only after an ACK.

## Gotchas
- **WAL files persist across runs.** `recover()` replays the existing `.dat` on startup, so a second demo run starts with the previous run's data (index sizes and query counts change). Delete the `.dat` files for a deterministic run. An unrecognised header reinitialises the log, and the old contents are discarded.
- **2PC is not actually logged.** `LogType` has `LOG_PREPARE/COMMIT/ABORT`, and `recover()` handles them (re-staging PREPAREs that never resolved), but `WorkerServer` currently writes only `LOG_DATA` (via `log_or_warn` at commit time). PREPARE staging is memory-only. If you add tx durability, write `LOG_PREPARE` in the PREPARE handler and `LOG_COMMIT`/`LOG_ABORT` in Phase 2.
- **Migration is not durable on the source.** `extract_and_remove_range` removes records from the source's index but not from its WAL, so a restart of the source resurrects migrated records. Sends to the target are also unchecked.
- **Index key collisions:** the map is keyed by Z-order code alone, so two records that quantize to the same millimetre cell overwrite each other. 2PC refuses coordinates outside about [-1000, 1097], but `CMD_INGEST` and `CMD_REPLICATE` skip that check, so there they are clamped.
- **Buffered records are invisible to queries** until the pool flushes. The demo uses `BufferPool(1, ...)` so every insert is visible immediately. `recover()` inserts straight into the index, bypassing the pool.
- **Shard boundary in the cluster tests:** `1<<62` is z's top bit, so z < 48.576 goes to 8081 and anything higher goes to 8083. After TEST 3, 8083 owns two non-contiguous ranges.
- **Workers are single-client:** the accept loop serves one connection to completion before the next. So don't make a worker call itself synchronously, and keep connections short. This is why `MortonClient` opens a connection per operation. `forward_to_standby` runs while `tx_mutex` is held in the commit path.
- **Connections are a limited resource:** each 2PC transaction opens a connection, and a closed one holds its local port in `TIME_WAIT` for about 30 s. macOS has 16,384 ephemeral ports, so one `insert` per point starts failing with `cannot reach Node` at around 16,000 transactions, and queries fail too. Use `insert_batch` for bulk loads. One bad point aborts its whole batch.
- `NetworkClient` checks every `send`/`recv` and ignores `SIGPIPE`, but most in `WorkerServer` are unchecked. A worker writing to a client that has gone can still die from `SIGPIPE`. Code that adds new network paths should require full-size reads, as `NetworkClient` does.
- **`send_record` has no ack:** the first write after a peer closes can still return true. The SDK uses 2PC instead, so its `insert` result is real.
- **The optimizer has no row statistics:** it estimates cost from key span alone, so it can split a box over sparse data where the extra round trips cost more than they save.

## Conventions
- Each component is a `Name.h` + `Name.cpp` pair at the repo root, using `#pragma once`. Wire and disk structs live inside `#pragma pack(push,1)`.
- Log lines are prefixed `[Coordinator]`, `[Worker Node <port>]`, `[Primary Worker <port>]`, `[Standby Worker <port>]`, or `[WAL]`.
- Comments explain *why* (invariants, ordering), not *what*.
- Commit messages use Conventional Commits with a scope, e.g. `fix(2pc,wal,sharding): ...`, `feat(coordinator): ...`.
- Keep [README.md](README.md) in step with architecture changes. If the two disagree, trust the code.
