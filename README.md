# Distributed Spatial Database (Advanced DBMS Project)

A small C++17 prototype of a sharded spatial database for 3D point data, such as mmWave radar returns and LiDAR scans. It combines a memory-mapped write-ahead log, a Z-order (Morton) index, a TCP worker per shard, two-phase commit, primary-to-standby replication, failover, shard splitting, a client SDK with a query optimizer, and a loader for real KITTI LiDAR frames.

> Status: prototype / coursework. POSIX only (macOS/Linux), with no dependencies. See [Known limitations](#known-limitations).

## Quick start

There is no build system. With a C++17 compiler on macOS or Linux:

```bash
g++ -std=c++17 -O2 -Wall -pthread -o spatial_db \
    main.cpp Coordinator.cpp NetworkClient.cpp WorkerServer.cpp \
    BufferPool.cpp SpatialIndex.cpp MortonCode.cpp WriteAheadLog.cpp \
    MortonClient.cpp QueryOptimizer.cpp KittiLoader.cpp

rm -f kitti_wal.dat   # for a clean run: existing WAL files are replayed on startup
./spatial_db          # or: ./spatial_db path/to/frame.bin (default 000000.bin)
```

The demo needs port 8081 free and a KITTI Velodyne frame. `000000.bin` is not in the repository, so supply your own copy. The demo starts one in-process worker, ingests the frame, and runs a bounding-box query over x ∈ [-10, 10], y ∈ [5, 25], z ∈ [-2, 3]. With the 115,384-point frame used during development, the output ends with:

```
-> Points Ingested: 115384 / 115384
[PASS] Successfully extracted 20264 3D spatial points from KITTI dataset.
```

Ingestion took about 3 seconds on the development Mac. The run creates `kitti_wal.dat` (100 MB), which is a generated file and should not be committed.

## Architecture

```
        ┌───────────────────────────────┐
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
| Dataset | [KittiLoader.h](KittiLoader.h), [KittiLoader.cpp](KittiLoader.cpp) | Reads KITTI `.bin` frames and loads them in batches |
| Demo | [main.cpp](main.cpp) | Single-worker KITTI ingestion and query |

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
                           .execute();
if (result.ok) { /* use result.records */ }
```

Use `insert_batch` for bulk loads. Each transaction opens a TCP connection, and a closed connection holds its local port in `TIME_WAIT` for about 30 seconds. One `insert` per point therefore runs out of ephemeral ports after roughly 16,000 transactions on macOS, and later inserts and queries fail. `KittiLoader` sends 1,000 points per transaction, so a 115,384-point frame needs 116 connections.

## How it works

### Routing

A coordinate is quantized with `(v + 1000) * 1000`, then its three 21-bit values are interleaved into a Z-order key. Each shard owns an inclusive key range, and the coordinator finds the owner with a linear scan. Quantization is monotone, so every point in a box has a key between the keys of the box's two corners.

### Two-phase commit

`Coordinator::execute_2pc_transaction` groups records by owning shard and refuses the whole transaction if any record can't be routed.

1. **Prepare:** the coordinator sends one `CMD_PREPARE` per record, only to the owning shards. The worker votes abort if a coordinate is NaN, infinite or outside the quantizer's range of about [-1000, 1097]. Otherwise it stages the record in memory and votes commit.
2. **Commit or abort:** if every vote is commit, each worker writes its staged records to the WAL, inserts them into the buffer pool, and forwards them to its standby. A missing ACK is retried once on a fresh connection, which is safe because commit is idempotent. If any vote is abort, every connected worker discards the staged records, so one bad point aborts the whole batch.

### Queries

`QueryOptimizer::optimize` returns one of three plans: a single key range, up to 8 disjoint sub-boxes, or `INVALID_PLAN` for an inverted or NaN box. It splits the box only when that cuts the total key span at least 4x. `Coordinator::execute_query` asks each overlapping shard once per sub-box, and if any shard fails, the whole query fails rather than returning partial rows.

### Replication and failover

`WorkerServer::set_standby_node(ip, port)` gives a worker a standby. After a local ingest or commit, the primary sends each record as `CMD_REPLICATE`, and the standby writes it to its own WAL and index without forwarding it. There is no acknowledgement.

`failover_primary_to_standby` remaps the failed node's shards to the standby in the coordinator's shard map. It does not detect failures, so something else has to call it.

### Shard splitting

`split_shard` halves the old node's key range and sends `CMD_MIGRATE_RANGE`. The old worker extracts the upper half and sends it to the target. If the target can't be reached, the worker restores the records and replies with a NACK. The coordinator updates the shard map only after an ACK.

### Recovery

On startup, a worker replays its WAL into the index. An unrecognised WAL header reinitialises the log and discards its contents.

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
| `CMD_HEARTBEAT` | 0x0C | not implemented | declared but not handled |
| `CMD_SPLIT_SHARD` | 0x0D | not implemented | declared but not handled |

## Testing

There are no unit tests, and [main.cpp](main.cpp) only exercises the single-worker KITTI path. Its output is not a strict check, because it prints `[PASS]` for 0 rows and exits 0 after a failed query, so compare the printed counts.

The cluster integration tests were removed from `main.cpp` in commit `6d4c44a`. They cover cross-shard 2PC, replication, failover, splitting and the SDK, and are still available as `git show a20327a:main.cpp`. To run them, build that file in place of `main.cpp` and without `KittiLoader.cpp`, with ports 8081–8083 free. Two of their abort cases use `x = -1`, which is now valid, so change both to `5000.0f` first. With that change, all 18 checks pass.

## Known limitations

- **2PC is not durable:** staged transactions live only in memory, workers log only committed data, and the coordinator keeps no decision log. A crash between phases leaves the outcome undefined.
- **Replication:** one connection per record, with no acknowledgement and no catch-up for an offline standby.
- **Failover:** a manual remap in the coordinator, with no failure detection.
- **Migration:** records leave the source's index but not its WAL, so restarting the source brings migrated records back. Sends to the target are unchecked.
- **Index:** keys come from coordinates alone, so two records in the same millimetre cell overwrite each other. The `CMD_INGEST` and `CMD_REPLICATE` paths skip range validation, so out-of-range coordinates are clamped there.
- **Visibility:** buffered records can't be queried until the pool flushes. The demo uses a pool capacity of 1 so inserts are visible immediately.
- **Optimizer:** it has no row statistics, so it can split a box over sparse data when the extra round trips cost more than they save.
- **Networking:** each worker serves one connection at a time, most `send` and `recv` calls in `WorkerServer` are unchecked, and a worker can die from `SIGPIPE` if a client disappears mid-reply.
- **WAL:** the file has a fixed size and doesn't wrap or grow.

## Possible next steps

- Bring the cluster integration tests back and make the demo fail on an unexpected row count.
- Add a build file (CMake or Makefile).
- Log 2PC state (`LOG_PREPARE`, `LOG_COMMIT`, `LOG_ABORT`) so transactions survive a crash.
- Replace per-record replication with a persistent, acknowledged stream with catch-up.
- Make migration durable on the source.
- Give the optimizer row statistics.
