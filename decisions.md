# Decisions

A running log of work on this project and the reasons behind it, newest first.

## 2026-11-04: Applied the review fixes

All 16 findings from the review below are addressed. They are committed on the branch `fix/layer5-sdk-cluster-routing` rather than `main`, so you can review them before merging. The `SplitShardPayload` edit in `NetworkProtocol.h` is left out of the commit because it is your own work in progress.

### Decisions

**The SDK goes through the Coordinator (findings 3, 6).** You didn't pick an option, so I used my recommendation. It restores routing, 2PC and failover, which were the point of Layer 4. `MortonClient` now takes a `Coordinator&`, and `connect()` and `disconnect()` are gone because each operation opens its own short connection. This changes the SDK's API, and a client now needs a `Coordinator` in the same process.

**`FULL_SHARD_SWEEP` was replaced, not implemented (findings 1, 2).** This differs from what the review suggested. On a `std::map`, a range scan reads a subset of what a sweep reads, so a sweep can never be the cheaper plan. The choice that does matter is how many key ranges to scan, so the new `MORTON_SPLIT_RANGE_SCAN` plan splits the box into up to 8 narrower ranges. Cost is now key span rather than volume.

**Split thresholds are judgement calls.** Each sub-box costs a round trip per shard, so I capped it at 8 and required at least a 4x cut in key span. Neither number is tuned, and there are no row statistics, so sparse data can be split when it isn't worth it.

**Sub-boxes meet at adjacent floats.** Each split uses the last float below a cell boundary and the very next float. Any point lands in exactly one half, so results need no de-duplication.

**A query fails as a whole.** If one shard doesn't answer, `execute()` returns `ok=false` with no rows, because partial rows would look like a complete answer.

**TEST 4 runs last, on the failed-over and split cluster.** That is the hardest routing case, because 8083 owns two non-contiguous ranges and must be asked only once. `main.cpp` now exits 1 on any failed check.

**`send_record` stays and still has no ack.** TEST 1 uses it for the raw ingest path. Adding an ack would change the wire protocol for the migration path too.

### Verification

| Check | Before | After |
|---|---|---|
| Clean demo run | Printed success with no checks | 18 of 18 checks pass, exit 0 |
| Rerun with WALs kept | Not checked | 18 of 18 pass, exit 0 |
| Port 8083 already taken | Printed `VERIFIED`, exit 0 (tested on 8081) | Logs the bind error, 6 checks fail, exit 1 |
| ASan/UBSan | Clean | Clean |
| Write to a closed peer | Process killed, exit 141 | `send` fails and the process survives |
| Truncated query reply | Fake row at (0, 0, 0) | `ok=false`, no rows |
| 20 repeated connects | 5 sockets leaked | No leak |
| Copying the socket owner | Compiled, broke the original | Doesn't compile |
| Insert or query with node down | Not checked through the Coordinator | Both report failure |
| 3,000 random boxes, plan vs single scan | Not applicable | 0 row mismatches |

Across those 3,000 boxes, 2,670 used split plans and rows scanned fell from 156,015,317 to 15,185,335, about 10x. The tiny box from finding 2 went from scanning 94,604 rows to 0, while the grid-aligned cube correctly stayed a single range.

### Known limits left in place

- **Workers can still die from `SIGPIPE`.** Only `NetworkClient` is protected. A worker replying to a client that has gone is still exposed. It wasn't in the review, so I left it for you to decide.
- **Workers still serve one connection at a time.** The SDK avoids the blocking, but a raw `NetworkClient` that stays connected still blocks others.
- **`insert` can report success with an unconfirmed commit.** `execute_2pc_transaction` returns true once COMMIT is decided, even if a node never ACKs. It logs a warning, and this is existing behaviour.
- **`Coordinator` isn't thread-safe.** Using one from several threads needs a lock, which matters more now that the SDK depends on it.
- **`README.md` is still stale.** It wasn't in scope.

## 2026-11-04: Review of commit 676f7eb (Layer 5 SDK and optimizer)

This entry records what was reviewed, how, and why each call was made. Its findings are fixed in the entry above.

### Outcome

The demo's normal path works: on a clean run it returns the one expected point, and ASan/UBSan report nothing. The commit still has problems worth fixing before building on it:

- The optimizer's plan has no effect on how a query runs.
- The SDK bypasses sharding, 2PC and failover.
- The Layer 4 integration test was deleted.
- The demo prints `VERIFIED` even when its query fails.

No source files were changed. This file is the only addition to the repo.

### Decisions

#### 1. Report the bugs, don't fix them
The request was to review, debug and return an analysis, not to change the code. Most fixes also depend on design choices that are yours, such as whether the SDK should go through the Coordinator. Patching them first would have made those choices for you.

#### 2. Review the commit and the code it calls
The diff alone would miss most of the risk, because the new SDK is a thin wrapper. I also read `NetworkClient`, `WorkerServer`, `SpatialIndex`, `MortonCode`, `BufferPool` and the `Coordinator` interface.

Older bugs are included when the new SDK exposes them, and each is marked "pre-existing" below.

#### 3. Reproduce findings instead of relying on reading
Reading suggested more problems than turned out to be real, so each suspected runtime bug got a test where one was practical. Findings are labelled **Reproduced** or **By reading**.

#### 4. Keep testing outside the repo
Builds, WAL files and harnesses all stayed in the session scratchpad, because WAL files persist between runs and would have altered your `.dat` state. Harness scenarios used ports 18081–18086. The demo needs 8081, which was free.

#### 5. Leave the uncommitted `SplitShardPayload` edit out of scope
The change in `NetworkProtocol.h` isn't part of the commit, and CLAUDE.md describes that command as work in progress.

#### 6. Rank by what happens to a user of the system
Wrong results that look successful, and lost guarantees, rank above crashes. A crash is at least visible. Style issues rank last.

### Findings

#### High

| # | Finding | Where | Evidence |
|---|---|---|---|
| 1 | The optimizer's plan is never used. Only `main.cpp` reads it, and only to print it. `FULL_SHARD_SWEEP` doesn't exist as an execution path, so every query takes the same route. | [main.cpp:41](main.cpp#L41), [main.cpp:47](main.cpp#L47) | By reading; `plan_type` has no consumers |
| 2 | The cost rule uses box volume, which doesn't predict scan cost. Key span, which does, is computed and then ignored. | [QueryOptimizer.cpp:23](QueryOptimizer.cpp#L23) | Reproduced, see below |
| 3 | The SDK talks to one worker directly, so it has no routing, no 2PC and no failover. A point with `x < 0`, which 2PC rejects, is stored. A query sees only that worker's shard. | [MortonClient.cpp:32](MortonClient.cpp#L32) | Reproduced for `x < 0`; the rest by reading |
| 4 | TESTs 0–3 were deleted from `main.cpp`, which CLAUDE.md names as the only integration test. 2PC, replication, failover and splitting no longer run anywhere. | [main.cpp](main.cpp) | By reading; `main.cpp` no longer references `Coordinator` |
| 5 | The demo prints `VERIFIED` and exits normally without checking anything. With port 8081 taken, the worker still logged `Listening`, the query returned 0 rows, and `VERIFIED` printed. | [main.cpp:63](main.cpp#L63), [WorkerServer.cpp:98](WorkerServer.cpp#L98) | Reproduced |
| 6 | A held SDK connection blocks every other client, because workers serve one connection at a time. A second client's `connect` and `insert` both returned true, the record wasn't stored, and its query returned 0 rows after 5 s. | [MortonClient.cpp:24](MortonClient.cpp#L24) | Reproduced |

Finding 2 measured on 200,000 random points:

| Query | Plan chosen | Rows scanned | Rows returned |
|---|---|---|---|
| 0.2 m³ box across z = 48.576 | Index scan | 94,828 | 0 |
| Same size, away from that boundary | Index scan | 0 | 0 |
| 262 m cube aligned to the Morton grid | Full sweep | 58,973 | 58,973 |

The smallest box scanned 47% of the data and was called selective. The large aligned cube scanned only rows it returned and was called broad, so the rule is wrong in both directions.

#### Medium

| # | Finding | Where | Evidence |
|---|---|---|---|
| 7 | Writing to a closed connection kills the client process with `SIGPIPE`. The first insert returned true, and the next one ended the process with exit code 141. Pre-existing. | [NetworkClient.cpp:36](NetworkClient.cpp#L36) | Reproduced |
| 8 | `insert` returning true means nothing: `send_record` ignores `send` results and there is no ack. Pre-existing, but the SDK now presents it as a success flag. | [MortonClient.cpp:32](MortonClient.cpp#L32) | Reproduced |
| 9 | A truncated query reply produces fake data. With 2 records promised and 1 sent, the client returned a second row at (0, 0, 0). Pre-existing. | [NetworkClient.cpp:54](NetworkClient.cpp#L54) | Reproduced |
| 10 | `execute()` returns an empty list for no matches, a timeout, no connection and a missing bounding box alike, so callers can't tell failure from an empty result. | [MortonClient.cpp:18](MortonClient.cpp#L18) | Reproduced for timeout |
| 11 | The build line in CLAUDE.md no longer links, because it omits `MortonClient.cpp` and `QueryOptimizer.cpp`. Its clean-run `rm` also omits `nosql_layer5_wal.dat`. | [CLAUDE.md](CLAUDE.md) | Reproduced |

#### Low

| # | Finding | Where | Evidence |
|---|---|---|---|
| 12 | Copying a `MortonClient` shares its socket. When the copy was destroyed, the original's insert still returned true with `EBADF`. | [MortonClient.h:23](MortonClient.h#L23) | Reproduced |
| 13 | Calling `connect()` again leaks the old socket. 5 sockets stayed open even after `disconnect()`; the test's listen backlog capped how many connected. | [NetworkClient.cpp:13](NetworkClient.cpp#L13) | Reproduced |
| 14 | `INVALID_PLAN` is never produced. An inverted box gets an index plan and returns 0 rows, NaN becomes a full sweep, and a box entirely out of range reports `key_span=0`. | [QueryOptimizer.cpp:9](QueryOptimizer.cpp#L9) | Reproduced |
| 15 | `SpatialQueryBuilder` keeps a reference to the client's connection, so it dangles if it outlives the `MortonClient`. | [MortonClient.h:11](MortonClient.h#L11) | By reading |
| 16 | Small cleanups: the unused `<memory>` include, "64-bit" in a comment for a 63-bit key, and `intensity` being stored as `metadata`. | [MortonClient.h](MortonClient.h), [QueryOptimizer.cpp](QueryOptimizer.cpp) | By reading |

### Checked and not reported as bugs

- **Range query correctness:** the Morton key is monotone in each axis, so scanning between the two corner keys can't miss a point. The scan can be slow, as finding 2 shows, but it stays correct.
- **Worker shutdown:** I suspected a use-after-free when `main` returns while the worker thread is detached. ASan didn't report one, and the thread touches no members after `accept` fails.
- **TSan abort (exit 134):** the only race reported was on `std::cout` logging. I expect the abort is TSan's macOS default after any report rather than a program failure, though I didn't confirm that setting.
- **Same-cell overwrite:** two inserts in one millimetre cell kept only the second. CLAUDE.md already documents this.

### Suggested fix order

1. **Restore the integration test (4, 5).** Bring TESTs 0–3 back alongside the Layer 5 test and make failures exit non-zero, so every later fix has a safety net.
2. **Make failures visible (7, 8, 9, 10).** Suppress `SIGPIPE`, check `send` and `recv` sizes, and let `execute()` report errors.
3. **Decide the SDK's place in the cluster (3, 6).** This is your design choice. Routing through the Coordinator keeps the Layer 4 guarantees; a direct client would need to be documented as single-shard.
4. **Make the optimizer real (1, 2, 14).** Base cost on key span or row estimates, and either add a real sweep path or remove the plan type.
5. **Tidy up (11, 12, 13, 15, 16).** Most of these are small, isolated changes.
