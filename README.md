# Distributed Spatial Database (Advanced DBMS Project)

A small C++17 prototype of a sharded, crash-aware spatial database for 3D point data (for example mmWave radar returns). It layers a memory-mapped write-ahead log, a buffered Z-order (Morton) index, a TCP wire protocol, and a two-phase-commit coordinator.

> Status: prototype / coursework. POSIX only (macOS/Linux). See [Known limitations](#known-limitations).

## Architecture

```
            ┌────────────────────────────┐
            │        Coordinator         │  2PC driver, Morton-range shard routing
            └──────┬──────────────┬──────┘
        TCP (binary protocol)     │
        ┌──────────▼───┐   ┌──────▼───────┐
        │ WorkerServer │   │ WorkerServer │   one per shard
        │   :8081      │   │   :8082      │
        │ ┌──────────┐ │   │ ┌──────────┐ │
        │ │BufferPool│ │   │ │BufferPool│ │   batches inserts
        │ └────┬─────┘ │   │ └────┬─────┘ │
        │ ┌────▼─────┐ │   │ ┌────▼─────┐ │
        │ │SpatialIdx│ │   │ │SpatialIdx│ │   std::map keyed by Z-order code
        │ └──────────┘ │   │ └──────────┘ │
        │ ┌──────────┐ │   │ ┌──────────┐ │
        │ │   WAL    │ │   │ │   WAL    │ │   mmap-backed append-only file
        │ └──────────┘ │   │ └──────────┘ │
        └──────┬───────┘   └──────────────┘
               │ CMD_REPLICATE (optional)
        ┌──────▼───────┐
        │ Standby node │
        └──────────────┘
```

| Layer | Files | Responsibility |
|---|---|---|
| Record | [SpatialRecord.h](SpatialRecord.h) | Packed 24-byte record: `timestamp`, `x`, `y`, `z`, `metadata` |
| Storage | [WriteAheadLog.h](WriteAheadLog.h), [WriteAheadLog.cpp](WriteAheadLog.cpp) | `mmap`'d fixed-size file; `append` claims a slot with an atomic counter |
| Buffering | [BufferPool.h](BufferPool.h), [BufferPool.cpp](BufferPool.cpp) | Mutex-protected batch; flushes to the index when it reaches capacity |
| Index | [SpatialIndex.h](SpatialIndex.h), [SpatialIndex.cpp](SpatialIndex.cpp), [MortonCode.cpp](MortonCode.cpp) | Coordinates are quantized (×1000), interleaved into a 63-bit Z-order key, stored in a `std::map` |
| Cluster | [Coordinator.h](Coordinator.h), [Coordinator.cpp](Coordinator.cpp), [NodeConfig.h](NodeConfig.h) | Morton-range shard table, query routing, 2PC coordination |
| Network | [NetworkProtocol.h](NetworkProtocol.h), [NetworkClient.h](NetworkClient.h), [NetworkClient.cpp](NetworkClient.cpp), [WorkerServer.h](WorkerServer.h), [WorkerServer.cpp](WorkerServer.cpp) | Packed binary protocol over POSIX TCP; worker request loop |
| Demo | [main.cpp](main.cpp) | Starts two workers in-process and runs one cross-shard 2PC transaction |

## Wire protocol

Every message is a packed `PacketHeader` (`uint8_t command`, `uint32_t payload_len`) followed by a fixed-size struct. Defined in [NetworkProtocol.h](NetworkProtocol.h).

| Command | Code | Direction | Payload |
|---|---|---|---|
| `CMD_INGEST` | 0x01 | client → worker | `SpatialRecord` (applied immediately, no 2PC) |
| `CMD_QUERY` | 0x02 | client → worker | `QueryRequest` (x/y/z min/max) |
| `CMD_QUERY_RESP` | 0x03 | worker → client | array of `SpatialRecord` |
| `CMD_ACK` | 0x04 | worker → coordinator | none |
| `CMD_PREPARE` | 0x05 | coordinator → worker | `TxPacket` (`tx_id` + record) |
| `CMD_VOTE_COMMIT` | 0x06 | worker → coordinator | none |
| `CMD_VOTE_ABORT` | 0x07 | worker → coordinator | none |
| `CMD_COMMIT` | 0x08 | coordinator → worker | `uint64_t tx_id` |
| `CMD_ABORT` | 0x09 | coordinator → worker | `uint64_t tx_id` |
| `CMD_REPLICATE` | 0x0A | primary → standby | `SpatialRecord` |

## Two-phase commit

Driven by `Coordinator::execute_2pc_transaction` ([Coordinator.cpp](Coordinator.cpp)):

1. **Phase 1 (prepare):** the coordinator connects to each participant and sends `CMD_PREPARE`. The worker validates the record. If `x < 0` it replies `CMD_VOTE_ABORT` and stages nothing. Otherwise it stages the record in memory (`staged_tx`) and replies `CMD_VOTE_COMMIT`.
2. **Phase 2:** if every vote is commit, the coordinator sends `CMD_COMMIT`, and each worker appends the staged record to its WAL, inserts it into the buffer pool, forwards it to its standby (if any), and replies `CMD_ACK`. Otherwise the coordinator sends `CMD_ABORT` and workers discard the staged record.

## Replication

A worker can be given a standby with `WorkerServer::set_standby_node(ip, port)`. After a local `CMD_INGEST` or `CMD_COMMIT`, the primary opens a connection and sends the record as `CMD_REPLICATE`. The standby appends it to its own WAL and index without forwarding it again. This is best-effort shipping, not full streaming replication (see limitations).

## Build and run

There is no build system yet. With a C++17 compiler on macOS or Linux:

```bash
g++ -std=c++17 -O2 -pthread -o spatial_db \
    main.cpp Coordinator.cpp NetworkClient.cpp WorkerServer.cpp \
    BufferPool.cpp SpatialIndex.cpp MortonCode.cpp WriteAheadLog.cpp

./spatial_db
```

The demo starts workers on ports 8081 and 8082, runs transaction `9001` with a record at `(45, 90, 15)`, and prints the coordinator and worker log lines for each phase, ending in `COMMITTED` or `ABORTED`. To see the abort path, change the record's `x` in [main.cpp](main.cpp) to a negative value.

Running creates `worker1_wal.dat` and `worker2_wal.dat` (100 MB each, sparse). They are generated files and should not be committed.

## Known limitations

- **Build may currently fail:** [WorkerServer.cpp](WorkerServer.cpp) calls `SpatialIndex::range_query`, but [SpatialIndex.h](SpatialIndex.h) only declares `insert`. Check that before relying on the build command above.
- **No 2PC durability:** staged transactions live only in memory and the coordinator keeps no decision log. A worker or coordinator crash between phases leaves the outcome undefined, and there is no recovery on restart.
- **Coordinator loop stops early:** on the first connect failure it breaks out of phase 1, so later workers are never contacted (abort is still sent to all clients, but unconnected ones just fail).
- **WAL:** no record framing, checksum, or recovery pass. It does not wrap or grow, and `append` has no bounds check.
- **Replication:** one connection per record, no acknowledgement, no catch-up for an offline standby, and `forward_to_standby` runs while `tx_mutex` is held in the commit path.
- **Index:** negative coordinates are folded with `abs()`, so `(-x)` and `x` map to the same key. The index is also keyed by Z-order code alone, so two records that quantize to the same key overwrite each other.
- **Networking:** `recv`/`send` return values are mostly unchecked, there is one blocking client at a time per worker, and the protocol is host-endian with packed structs, so it assumes the same architecture on both ends.

## Possible next steps

- Add a build file (CMake or Makefile) and a `.gitignore` for `*.dat` and the binary.
- Persist 2PC state (prepared / committed / aborted) in the WAL and add a recovery pass.
- Replace fire-and-forget replication with a persistent stream, acknowledgements, and LSN-based catch-up.
- Add `range_query` to `SpatialIndex` and tests for the Morton encoding and shard routing.
