# Distributed Spatial Database (Advanced DBMS Project)

A small C++17 prototype of a sharded spatial database for 3D point data, such as mmWave radar returns and LiDAR scans. It combines a memory-mapped write-ahead log, a Z-order (Morton) index, a TCP worker per shard, two-phase commit, primary-to-standby replication, failover, shard splitting, a client SDK with a query optimizer, a loader for real KITTI LiDAR frames, and an interactive shell with its own query language.

> Status: prototype / coursework. POSIX only (macOS/Linux), with no dependencies. See [Known limitations](#known-limitations).

## Quick start

There is no build system. With a C++17 compiler on macOS or Linux:

```bash
g++ -std=c++17 -O2 -Wall -pthread -o spatial_db \
    main.cpp Coordinator.cpp NetworkClient.cpp WorkerServer.cpp \
    BufferPool.cpp SpatialIndex.cpp MortonCode.cpp WriteAheadLog.cpp \
    MortonClient.cpp QueryOptimizer.cpp KittiLoader.cpp \
    QueryLexer.cpp QueryParser.cpp QueryExecutor.cpp LineEditor.cpp

rm -f kitti_wal.dat   # for a clean run: existing WAL files are replayed on startup
./spatial_db          # demo; or: ./spatial_db path/to/frame.bin (default 000000.bin)
./spatial_db --shell  # interactive shell; see "Query shell" below
./spatial_db --shell --shards 4   # the same with 4 workers
```

The demo needs port 8081 free and a KITTI Velodyne frame. `000000.bin` is not in the repository, so supply your own copy. The demo starts one in-process worker, ingests the frame, and runs a bounding-box query over x ∈ [-10, 10], y ∈ [5, 25], z ∈ [-2, 3]. With the 115,384-point frame used during development, the output ends with:

```
-> Points Ingested: 115384 / 115384
[PASS] Successfully extracted 20264 3D spatial points from KITTI dataset.
```

Ingestion took about 3 seconds on the development Mac. The run creates `kitti_wal.dat` (100 MB), which is a generated file and should not be committed.

## Query shell

`./spatial_db --shell` starts its workers and opens a prompt. Data from an earlier shell session is available straight away, because each worker keeps its own `shard_<port>_wal.dat`. The demo uses `kitti_wal.dat`, so the two don't share data. Logs go to `spatial_shell.log`, which is overwritten on each start, so the terminal shows only results.

```
spatial> LOAD '000000.bin';
loaded 115384 of 115384 points in 3109 ms
spatial> COUNT BOX x[-10,10] y[5,25] z[-2,3] WHERE intensity > 100;
4962
spatial> FIND BOX x[-10,10] y[5,25] z[-2,3] WHERE intensity > 100 LIMIT 2;
(-9.929, 5.033, -1.752)  metadata=102
(-9.922, 5.069, -1.754)  metadata=104
2 rows
```

Every command ends with `;`, so a command can span several lines. Keywords ignore case.

| Command | What it does |
|---|---|
| `FIND BOX x[a,b] y[a,b] z[a,b] [WHERE ...] [LIMIT n];` | Returns matching points. An axis left out covers its full range. |
| `FIND ALL [WHERE ...] LIMIT n;` | Scans everything. `LIMIT` is required, so a typo can't pull back the whole database. |
| `COUNT BOX ... [WHERE ...];` or `COUNT ALL [WHERE ...];` | Returns only the number of matches. |
| `EXPLAIN FIND ...;` or `EXPLAIN COUNT ...;` | Shows the optimizer's plan without running the query. |
| `INSERT POINT (x, y, z) [METADATA n];` | Inserts one point through 2PC. |
| `LOAD 'file.bin';` | Loads a KITTI frame in batches. |
| `SHOW SHARDS;` | Lists each worker's point count and key range. |
| `REBALANCE;` | Moves points so every worker holds an equal share. See [Adding workers](#adding-workers). |
| `HELP;` and `EXIT;` | Shows the commands, or quits. |

`WHERE` compares `metadata` using `= != < <= > >=` against a whole number, with up to 4 conditions joined by `AND`. `intensity` is an alias for `metadata`, which holds reflectance scaled to 0–255 for KITTI data.

A `FIND` with no `LIMIT` prints the first 50 rows and the total. Rows come back in key order, not sorted by any field.

At a terminal, you can edit the line you're typing:

| Key | Action |
|---|---|
| ← → | Move the cursor |
| ↑ ↓ | Previous or next command from this session |
| Backspace, Delete | Delete a character |
| Home or Ctrl-A, End or Ctrl-E | Jump to the start or end of the line |
| Ctrl-U, Ctrl-K | Delete to the start or end of the line |
| Ctrl-C | Drop what you're typing |
| Ctrl-D | Quit, on an empty line |

Commands longer than the terminal scroll sideways. Only ASCII can be typed, and history isn't kept between sessions.

## Adding workers

`--shards N` starts N workers (up to 16) on consecutive ports from 8081, each with its own WAL. The number is remembered, so later runs of `./spatial_db --shell` start the same workers.

```
./spatial_db --shell --shards 4
spatial> LOAD '000000.bin';
spatial> SHOW SHARDS;
node 8081: 115310 points, keys 0 to 2305843009213693950
node 8082: 74 points, keys 2305843009213693951 to 4611686018427387901
node 8083: 0 points, keys 4611686018427387902 to 6917529027641081852
node 8084: 0 points, keys 6917529027641081853 to 9223372036854775807
spatial> REBALANCE;
rebalanced 115384 points across 4 workers (86538 moved)
spatial> SHOW SHARDS;
node 8081: 28846 points, keys 0 to 1152648248193645857
...
```

The first split is uneven because workers start with equal slices of the key space, while real points cluster in a narrow band of keys. `REBALANCE;` reads the keys every worker holds, cuts them into equal groups, moves the points, confirms the total hasn't changed, then saves the new boundaries to `shard_map.dat`. Query results stay the same throughout.

- **More workers:** restart with a higher `--shards N`, then run `REBALANCE;`. New workers own no keys until you do.
- **Fewer workers:** refused, because the workers left out would take their points with them. To start again, delete `shard_*_wal.dat` and `shard_map.dat`.
- **New data:** it can make workers uneven again, so run `REBALANCE;` after a large `LOAD`.

More workers spread the data but don't add speed yet: the coordinator asks shards one at a time, and all workers run in one process on one machine.

## Architecture

```
        ┌───────────────────────────────┐
        │     Query shell (--shell)     │  lexer, parser, executor
        └───────────────┬───────────────┘
                        │ parsed commands
        ┌───────────────▼───────────────┐
        │  Application / KittiLoader    │
        └───────────────┬───────────────┘
                        │ insert, insert_batch, query()
        ┌───────────────▼───────────────┐
        │ MortonClient + QueryOptimizer │  SDK, plans each query
        └───────────────┬───────────────┘
                        │
        ┌───────────────▼───────────────┐
        │          Coordinator          │  shard map, 2PC, split, failover
        └───────┬───────────────┬───────┘
                │  TCP (binary protocol)
        ┌───────▼──────┐ ┌──────▼───────┐
        │ WorkerServer │ │ WorkerServer │  one per shard
        │ ┌──────────┐ │ │ ┌──────────┐ │
        │ │   WAL    │ │ │ │   WAL    │ │  mmap'd log, replayed on startup
        │ ├──────────┤ │ │ ├──────────┤ │
        │ │BufferPool│ │ │ │BufferPool│ │  batches inserts
        │ ├──────────┤ │ │ ├──────────┤ │
        │ │SpatialIdx│ │ │ │SpatialIdx│ │  std::map keyed by Z-order code
        │ └──────────┘ │ │ └──────────┘ │
        └───────┬──────┘ └──────────────┘
                │ CMD_REPLICATE (optional)
        ┌───────▼──────┐
        │ Standby node │
        └──────────────┘
```

| Layer | Files | Responsibility |
|---|---|---|
| Record | [SpatialRecord.h](SpatialRecord.h) | Packed 24-byte record: `timestamp`, `x`, `y`, `z`, `metadata` |
| Storage | [WriteAheadLog.h](WriteAheadLog.h), [WriteAheadLog.cpp](WriteAheadLog.cpp) | Fixed-size `mmap` file with a header and typed entries; appends are mutex-guarded and fail when the log is full |
| Buffering | [BufferPool.h](BufferPool.h), [BufferPool.cpp](BufferPool.cpp) | Batches records and flushes them to the index at capacity |
| Index | [SpatialIndex.h](SpatialIndex.h), [SpatialIndex.cpp](SpatialIndex.cpp), [MortonCode.h](MortonCode.h), [MortonCode.cpp](MortonCode.cpp) | Quantizes coordinates to millimetres, interleaves them into a 63-bit Z-order key, and stores records in a `std::map` with range queries |
| Cluster | [Coordinator.h](Coordinator.h), [Coordinator.cpp](Coordinator.cpp), [NodeConfig.h](NodeConfig.h) | Shard map, routing, 2PC, cross-shard queries, shard splitting, failover |
| Network | [NetworkProtocol.h](NetworkProtocol.h), [NetworkClient.h](NetworkClient.h), [NetworkClient.cpp](NetworkClient.cpp), [WorkerServer.h](WorkerServer.h), [WorkerServer.cpp](WorkerServer.cpp) | Packed binary protocol over POSIX TCP; WAL recovery and the worker request loop |
| SDK | [MortonClient.h](MortonClient.h), [MortonClient.cpp](MortonClient.cpp), [QueryOptimizer.h](QueryOptimizer.h), [QueryOptimizer.cpp](QueryOptimizer.cpp) | Client API and key-range query planning |
| Query language | [QueryLexer.h](QueryLexer.h), [QueryLexer.cpp](QueryLexer.cpp), [QueryParser.h](QueryParser.h), [QueryParser.cpp](QueryParser.cpp), [QueryExecutor.h](QueryExecutor.h), [QueryExecutor.cpp](QueryExecutor.cpp), [LineEditor.h](LineEditor.h), [LineEditor.cpp](LineEditor.cpp) | Turns typed text into tokens and then a `Command`, runs it through the SDK, and holds the shell loop with its line editing |
| Dataset | [KittiLoader.h](KittiLoader.h), [KittiLoader.cpp](KittiLoader.cpp) | Reads KITTI `.bin` frames and loads them in batches |
| Demo | [main.cpp](main.cpp) | Single-worker KITTI ingestion and query, plus the `--shell` entry point |

## Using the SDK

`MortonClient` goes through the `Coordinator`, so writes get routing and 2PC, and reads cover every shard. A node must be registered twice: `register_node` gives its address and `register_shard` gives the keys it owns.

```cpp
Coordinator coordinator;
coordinator.register_node(8081, {"127.0.0.1", 8081});
coordinator.register_shard(8081, 0, MAX_MORTON_KEY);

MortonClient client(coordinator);

client.insert(1.5f, 2.5f, 3.5f);   // one record, one transaction
client.insert_batch(records);      // many records, one all-or-nothing transaction

QueryResult result = client.query()
                           .bounding_box(-10, 10, 5, 25, -2, 3)
                           .where(FILTER_GT, 100)   // on metadata; several calls are ANDed
                           .limit(5)
                           .execute();
if (result.ok) { /* use result.records */ }

uint64_t total = client.query().all().execute_count().count;
```

Use `insert_batch` for bulk loads. Each transaction opens a TCP connection, and a closed connection holds its local port in `TIME_WAIT` for about 30 seconds. One `insert` per point therefore runs out of ephemeral ports after roughly 16,000 transactions on macOS, and later inserts and queries fail. `KittiLoader` sends 1,000 points per transaction, so a 115,384-point frame needs 116 connections.

## How it works

### Routing

A coordinate is quantized with `(v + 1000) * 1000`, then its three 21-bit values are interleaved into a Z-order key. Each shard owns an inclusive key range, and the coordinator finds the owner with a linear scan. Quantization is monotone, so every point in a box has a key between the keys of the box's two corners.

### Two-phase commit

`Coordinator::execute_2pc_transaction` groups records by owning shard and refuses the whole transaction if any record can't be routed.

1. **Prepare:** the coordinator sends one `CMD_PREPARE` per record, only to the owning shards. The worker votes abort if a coordinate is NaN, infinite or outside the quantizer's range of about [-1000, 1097]. Otherwise it writes the record to its WAL as `LOG_PREPARE`, stages it in memory, and votes commit. A worker whose WAL is full votes abort.
2. **Decide:** if every vote is commit, the coordinator appends the transaction id to its decision log and syncs the file. This write is the commit point. If it fails, the transaction aborts.
3. **Commit or abort:** on commit, each worker writes one `LOG_COMMIT` entry, syncs its WAL, inserts the staged records into the buffer pool, and forwards them to its standby. A missing ACK is retried once on a fresh connection, which is safe because commit is idempotent. If any vote is abort, every connected worker writes `LOG_ABORT` and discards the staged records, so one bad point aborts the whole batch.

The decision log holds commits only. A transaction that isn't in it is treated as aborted, so aborts need no write. The shell keeps its decisions in `tx_decisions.dat` and the demo in `kitti_tx.dat`; both are generated files.

### Queries

`QueryOptimizer::optimize` returns one of three plans: a single key range, up to 8 disjoint sub-boxes, or `INVALID_PLAN` for an inverted or NaN box. It splits the box only when that cuts the total key span at least 4x. `Coordinator::execute_filtered_query` asks each overlapping shard once per sub-box, and if any shard fails, the whole query fails rather than returning partial rows.

Filters, limits and counting all run on the worker. `COUNT` returns one number per shard, and those numbers are added, which is safe because sub-boxes never overlap. For `LIMIT`, the client asks each shard only for the rows it still needs, so the total is exact, and a worker stops scanning as soon as it has enough.

### Replication and failover

`WorkerServer::set_standby_node(ip, port)` gives a worker a standby. After a local ingest or commit, the primary sends each record as `CMD_REPLICATE`, and the standby writes it to its own WAL and index without forwarding it. There is no acknowledgement.

`failover_primary_to_standby` remaps the failed node's shards to the standby in the coordinator's shard map. It does not detect failures, so something else has to call it.

### Shard splitting

`split_shard` halves the old node's key range and sends `CMD_MIGRATE_RANGE`. The old worker extracts the upper half and sends it to the target, followed by a heartbeat. The target's reply to that heartbeat proves it applied everything before it, because a worker handles a connection in order. If that confirmation doesn't arrive, the source restores the records and replies with a NACK. The coordinator updates the shard map only after an ACK.

Halving the key range can move nothing at all on clustered data, which is why the shell uses `REBALANCE;` instead. It uses the same migration, but picks boundaries from the keys the workers actually hold.

### Recovery

On startup, a worker replays its WAL into the index. A `LOG_PREPARE` record is applied only if its `LOG_COMMIT` follows; one with neither a commit nor an abort is staged again as an unfinished transaction.

Once the workers are listening, `Coordinator::resolve_pending_transactions` asks each one for its unfinished transaction ids (`CMD_GET_PENDING_TX`). It sends `CMD_COMMIT` for the ids in the decision log and `CMD_ABORT` for the rest, then empties the log. This runs before the shell prompt or the demo's ingest, so no query sees half a batch. If a worker can't be reached, the log is kept and the next start tries again.

Transaction ids start from the process start time, so an id from an earlier run can't be reused while a worker still holds it.

After a migration, the source logs a `LOG_REMOVE_RANGE` entry so moved records don't reappear during replay. An unrecognised WAL header reinitialises the log and discards its contents.

## Wire protocol

Every message is a packed `PacketHeader` (`uint8_t command`, `uint32_t payload_len`) followed by a fixed-size struct, as defined in [NetworkProtocol.h](NetworkProtocol.h). Byte order is host-endian, so both ends must share an architecture.

| Command | Code | Direction | Payload |
|---|---|---|---|
| `CMD_INGEST` | 0x01 | client → worker | `SpatialRecord`, applied immediately with no 2PC or reply |
| `CMD_QUERY` | 0x02 | client → worker | `QueryRequest` (x/y/z min/max) |
| `CMD_QUERY_RESP` | 0x03 | worker → client | array of `SpatialRecord` |
| `CMD_ACK` | 0x04 | worker → coordinator | none |
| `CMD_PREPARE` | 0x05 | coordinator → worker | `TxPacket` (`tx_id` + record) |
| `CMD_VOTE_COMMIT` | 0x06 | worker → coordinator | none |
| `CMD_VOTE_ABORT` | 0x07 | worker → coordinator | none |
| `CMD_COMMIT` | 0x08 | coordinator → worker | `uint64_t tx_id` |
| `CMD_ABORT` | 0x09 | coordinator → worker | `uint64_t tx_id`; also the NACK for a failed migration |
| `CMD_REPLICATE` | 0x0A | primary → standby | `SpatialRecord`, no reply |
| `CMD_MIGRATE_RANGE` | 0x0B | coordinator → worker | `MigrateRangePayload` (key range + target address) |
| `CMD_HEARTBEAT` | 0x0C | worker → worker | none; replies `CMD_ACK`, which confirms a migration arrived |
| `CMD_SPLIT_SHARD` | 0x0D | not implemented | declared but not handled |
| `CMD_QUERY_FILTERED` | 0x0E | client → worker | `FilteredQueryRequest` (box, limit, count flag, up to 4 metadata filters) |
| `CMD_COUNT_RESP` | 0x0F | worker → client | `uint64_t` match count |
| `CMD_GET_KEYS` | 0x10 | coordinator → worker | none |
| `CMD_KEYS_RESP` | 0x11 | worker → coordinator | array of `uint64_t` keys, in order |
| `CMD_GET_PENDING_TX` | 0x12 | coordinator → worker | none |
| `CMD_PENDING_TX_RESP` | 0x13 | worker → coordinator | array of `uint64_t` ids of prepared, unfinished transactions |

## Testing

[tests/query_test.cpp](tests/query_test.cpp) checks the lexer and parser against a table of valid and invalid commands. It has its own `main`, so build it separately:

```bash
g++ -std=c++17 -Wall -I. -o query_test tests/query_test.cpp QueryLexer.cpp QueryParser.cpp
./query_test
```

The demo in [main.cpp](main.cpp) only exercises the single-worker KITTI path. Its output is not a strict check, because it prints `[PASS]` for 0 rows and exits 0 after a failed query, so compare the printed counts.

The cluster integration tests were removed from `main.cpp` in commit `6d4c44a`. They cover cross-shard 2PC, replication, failover, splitting and the SDK, and are still available as `git show a20327a:main.cpp`. To run them, build that file in place of `main.cpp` and without `KittiLoader.cpp`, with ports 8081–8083 free. Two of their abort cases use `x = -1`, which is now valid, so change both to `5000.0f` first. With that change, all 18 checks pass.

To test crash recovery, set `SPATIAL_DB_CRASH_POINT` to `after_prepare`, `after_decision` or `after_first_commit`. The process then exits at that point of the next transaction, without flushing anything. Start the shell again and check the counts: a crash at `after_prepare` must leave none of the batch, and the other two must leave all of it on every shard. `SHOW SHARDS;` gives the count per worker.

## Known limitations

- **2PC recovery:** an interrupted transaction is finished only when the process restarts, and its records stay staged until then. Workers sync their WAL at commit but not at prepare, so a power loss during Phase 1 can lose a prepared record; a process crash cannot. If a worker's WAL fills between its vote and the commit, that commit is applied but not durable. Deleting a decision file while keeping the WALs turns any unfinished commit into an abort.
- **Replication:** one connection per record, with no acknowledgement and no catch-up for an offline standby.
- **Failover:** a manual remap in the coordinator, with no failure detection.
- **Migration and rebalancing:** records move one worker at a time with no transaction around the whole operation. If a transfer fails, the shard map stays as it was, but points already moved can be missed by queries until `REBALANCE;` succeeds. A crash mid-rebalance can leave the same risk, and a target that took part of a failed transfer keeps those copies until the next `REBALANCE;`. Standbys aren't updated by a move. `SHOW SHARDS;` and `REBALANCE;` assume no other client is writing.
- **Index:** keys come from coordinates alone, so two records in the same millimetre cell overwrite each other. The `CMD_INGEST` and `CMD_REPLICATE` paths skip range validation, so out-of-range coordinates are clamped there.
- **Visibility:** buffered records can't be queried until the pool flushes. The demo uses a pool capacity of 1 so inserts are visible immediately.
- **Query shell:** it runs its own workers in one process, so it can't connect to a cluster that is already running. `WHERE` only compares `metadata`, and results have no `ORDER BY`. `WHERE` saves network traffic but not scanning, because the worker still walks every key between the box corners. A `FIND` with no `LIMIT` transfers every match, even though the shell prints only 50.
- **Timestamps:** `LOAD` and `INSERT` store a timestamp of 0, and the index key ignores it, so a later frame would overwrite earlier points in the same millimetre cell.
- **Optimizer:** it has no row statistics, so it can split a box over sparse data when the extra round trips cost more than they save.
- **Networking:** each worker serves one connection at a time, most `send` and `recv` calls in `WorkerServer` are unchecked, and a worker can die from `SIGPIPE` if a client disappears mid-reply.
- **WAL:** the file has a fixed size and doesn't wrap or grow. Each `LOAD` of a 115,384-point frame adds about 3.8 MB across the 100 MB WAL files, even if the points are already stored, and points moved by `REBALANCE;` are logged again on their new worker.

## Possible next steps

- Bring the cluster integration tests back and make the demo fail on an unexpected row count.
- Add a build file (CMake or Makefile).
- Replace per-record replication with a persistent, acknowledged stream with catch-up.
- Ask shards in parallel, so more workers mean faster queries.
- Make rebalancing atomic, so a failed one can't affect queries.
- Give the optimizer row statistics.
- Store real timestamps and add the timestamp to the index key, so several frames can coexist.
- Let the shell connect to an existing cluster.
