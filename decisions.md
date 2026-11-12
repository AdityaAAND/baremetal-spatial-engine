# Decisions

A running log of work on this project and the reasons behind it, newest first.

## 2026-11-12: Added dataset-tagged payloads and a live three.js viewer

`./spatial_db --shell --http` now serves a 3D view of the points a `LOAD` ingested, read back from the workers as JSON. Points carry `payload = (dataset_id << 16) | (intensity & 0xFFFF)` in the existing `metadata` field. The work is uncommitted, on `feat/2pc-durability`.

### Decisions

- **The payload equation was adopted as written.** It fits the existing `uint32 metadata`, so the record, WAL and wire record are unchanged. The dataset id is in the high bits so that one dataset is a contiguous range of values.
- **Filters gained a mask, applied on the worker.** Without it `WHERE intensity > 100` compares the whole packed value, and every point in dataset 1 or above passes. `intensity`, `dataset` and `metadata` are now three masks over one field. The alternative, rewriting filters in the parser, cannot express "intensity across all datasets".
- **`LOAD` tags points with dataset 1 or more; `INSERT` stores dataset 0.** You asked for the viewer to show only what `LOAD` ingested. The tag is what lets the worker tell the two apart, so `INSERT ... METADATA` is now limited to 0-65,535.
- **HTTP JSON bridge inside `spatial_db`, as you chose,** rather than an export file. The page stays live: it polls a count every 2 s and refetches when it changes.
- **Two flat JSON arrays, not one array per point.** They copy straight into typed arrays. A frame is about 3 MB.
- **Key spans travel as strings.** They reach 2^63, and a JavaScript number is exact only to 2^53.
- **A shared mutex between the HTTP thread and `REBALANCE;`.** `rebalance` replaces the shard map and moves points while it runs, and the `Coordinator` has no locking of its own. Other shell commands only read the map, so they take no lock.
- **The server polls for stop and is joined at exit.** A detached thread, as the workers use, could still be inside the `Coordinator` while `main` destroys it.
- **Loopback only, `GET` only.** The server has no authentication.
- **Z-order colour uses key rank, not `mkey >> 40`.** That shift keeps 23 bits, a cell of 16.4 m, and a KITTI frame spans about 10 cells per axis, so nearly every point would get the same colour. The server sorts rows by `point_key` and the page colours by array position, which costs no extra JSON and also enables the draw-in-key-order slider.
- **The slice is a second point layer, not a recolour of the base buffer.** It draws exactly the rows the server returned. Recolouring in place would need a client-side box test, which proves nothing about the database.
- **The plan label comes from the reply.** The optimizer does not always split, so the page shows the plan type that ran instead of assuming `MORTON_SPLIT_RANGE_SCAN`.
- **The decode is done in JavaScript with `>>>`.** A float vertex attribute is exact only to 2^24, and `>>` turns ids of 32,768 and above negative.
- **`FIND` now prints `dataset=` and `intensity=`** in place of the packed `metadata=` number, which was not in the plan but keeps rows readable.

### Verification

All run in scratch directories, so the repo's data files were not touched.

| Check | Result |
|---|---|
| Build with `-Wall` | No warnings |
| `tests/query_test.cpp`, with new rows for `dataset`, `AS DATASET` and the range errors | All passed |
| Shell, 1 worker: `LOAD`, `COUNT ALL;`, filtered count, box count | 115,384, 4,962 and 20,264 |
| `LOAD ... AS DATASET 2;` of the same frame | 115,384 in dataset 2, 0 in dataset 1 (the overwrite limit), filtered count still 4,962 |
| `LOAD ... AS DATASET 0;` and `INSERT ... METADATA 70000;` | Parse errors |
| `/api/points` before any `LOAD` | 0 rows |
| `/api/points` after `LOAD` plus one `INSERT` | 115,384 rows, while `COUNT ALL;` gives 115,385 |
| `/api/points` for the demo box, and with `imin=101` | 20,264 and 4,962, plan `MORTON_SPLIT_RANGE_SCAN` with 8 sub-boxes, as `EXPLAIN` prints |
| Row order of both replies, checked with `point_key` | 0 rows out of order |
| `POST`, unknown path, bad parameters, 9 KB request | 405, 404, 400, 400; the server kept answering |
| 25 `/api/points` requests during a 2-worker `REBALANCE;` that moved 57,692 points | All 25 returned 115,384 |
| Cluster tests from `a20327a` | 18 of 18 pass |
| Page in headless Chrome against the live server | Cloud, Z-order colouring, crimson slice and 8 sub-box wireframes all drew |

One fault was found and fixed during this: a request just over the 8 KB cap was accepted when its last chunk also held the blank line.

### Known limits left in place

- Datasets overwrite each other in shared millimetre cells, because the dataset id is not part of the index key.
- Points stored before this change are dataset 0, so the viewer hides them until they are loaded again.
- I tested the page only in headless Chrome with software rendering; mouse orbiting and the sliders were not exercised by hand.
- The demo (`./spatial_db` without `--shell`) now prints packed payloads such as 65620 in its sample rows.

## 2026-11-10: Made 2PC survive a crash

A transaction interrupted by a crash is now finished at the next start: committed on every shard if the coordinator had decided to commit, and aborted on every shard otherwise. Before this, staged records lived only in memory and the decision was written nowhere, so a crash between the phases could leave one shard with its part of a batch and another without. The work is uncommitted, on a new branch `feat/2pc-durability`.

### Decisions

- **Worker logs `LOG_PREPARE` before it votes yes.** A yes vote promises to commit later, and a promise held only in memory is lost in a crash. A full WAL now votes no.
- **One `LOG_COMMIT` per transaction replaces the per-record `LOG_DATA`.** The records are already in the log from the prepare, so writing them again would double WAL use. A frame load is 115,500 entries, against 115,384 before.
- **Worker logs `LOG_ABORT`,** so an aborted transaction doesn't come back as unfinished after a restart.
- **Coordinator writes a decision log before Phase 2.** That write is the commit point: a crash before it means abort, and a crash after it means commit.
- **Presumed abort.** Only commits are logged. A transaction missing from the log never passed Phase 1, so abort is the right answer and costs no write.
- **Startup resolves unfinished transactions.** The worker knows which are unfinished and the coordinator knows how they ended, so a new `CMD_GET_PENDING_TX` lets the coordinator ask and then send COMMIT or ABORT. It runs before any new work.
- **The decision log is emptied after a clean resolve,** which keeps it small without an end record per transaction. It is kept if any worker failed to answer.
- **Transaction ids start from the start time.** Ids now outlive the process in the WAL, and a reused id would add old staged records to a new transaction.
- **WAL sync at commit, not at prepare.** A sync per prepared record would be 115,384 syncs per frame. Load time stayed at about 3.2 s.
- **`SPATIAL_DB_CRASH_POINT` test switch.** The windows between phases are too short to hit by hand.
- **Branched from `fix/layer5-sdk-cluster-routing`, not `main`.** `main` doesn't build: it is missing commit `2e489e1`, which adds the `SpatialQueryBuilder` declarations to `MortonClient.h`. That branch also has the newer README.

### Verification

All run in a scratch directory, so the repo's data files were not touched.

| Check | Result |
|---|---|
| Build with `-Wall` | No warnings |
| Shell, 1 worker: `LOAD`, `COUNT ALL;`, filtered count | 115,384 and 4,962, and the same after a restart |
| Demo | 115,384 / 115,384 ingested, 20,264 rows |
| Crash at `after_prepare`, 2 shards, 1,000-point batch | After restart: 1 aborted, 0 and 0 points |
| Crash at `after_decision` | After restart: 1 committed, 107 and 893 points |
| Crash at `after_first_commit` | After restart: 1 committed, 107 and 893 points |
| Second restart after each crash | Same counts, nothing left to resolve, decision log empty |
| Cluster tests from `a20327a` | 18 of 18 pass |
| `tests/query_test.cpp` | 48 of 48 pass |

The no-crash baseline for the 1,000-point batch is 107 points on 8081 and 893 on 8082.

### Known limits left in place

- An unfinished transaction waits for a restart, as in plain 2PC.
- A power loss during Phase 1 can lose a prepared record. A process crash cannot.
- A WAL that fills between the vote and the commit leaves that commit applied but not durable.
- A transaction prepared before a `REBALANCE;` and resolved after it can land on a worker that no longer owns the key. This was not tested.

## 2026-11-05: Committed the session's work in layers

The uncommitted work went into five commits on `fix/layer5-sdk-cluster-routing`, grouped by layer so each one builds on its own: the worker and coordinator backend, the SDK builder, the query language with line editing, `main.cpp` with `--shell` and `--shards`, and the docs. Each commit was built from a clean export of that commit.

### Decisions

- **Held back the `SplitShardPayload` edit.** It's unfinished and nothing uses it, so it stays as an uncommitted change in `NetworkProtocol.h`.
- **Left out `000000.bin`.** Whether to commit it depends on the KITTI licence terms, so that call is still open.
- **Left the push to the user.** A push can't be fully undone once others see it.
- **Kept filtering and rebalancing in one backend commit.** They share `NetworkProtocol.h`, `WorkerServer.cpp`, `NetworkClient.cpp` and `Coordinator.cpp`, and splitting those files by hunk would have left a commit that doesn't build.

## 2026-11-05: Added --shards N and REBALANCE

`./spatial_db --shell --shards N` starts N workers, and `REBALANCE;` moves points so each holds an equal share. On `000000.bin` with 4 workers, the split goes from 115,310 / 74 / 0 / 0 to 28,846 each, with the same query results. These changes are uncommitted.

### Decisions

- **Boundaries come from the keys workers hold, not from the key range.** Halving the key range moved 0 points on this frame. The coordinator collects every key through a new `CMD_GET_KEYS`, which is about 0.9 MB for a frame.
- **Fixed moved points coming back after a restart.** The source now logs `LOG_REMOVE_RANGE`. This was a known gap in `split_shard`, so it benefits too.
- **Confirmed transfers with `CMD_HEARTBEAT`.** `CMD_INGEST` has no reply, so the source could never tell whether the target stored anything. A worker handles a connection in order, so an ACK to a trailing heartbeat proves everything before it was applied. This reused a command that was declared but unused.
- **Saved boundaries to `shard_map.dat`,** written to a temp file then renamed so a crash can't leave half a file. Without it, a restart would forget them and queries would miss points.
- **Growing is allowed and shrinking is refused.** New workers own no keys until `REBALANCE;`, so growing is safe. Dropping a worker would strand its points.
- **The saved map sets the default worker count,** so plain `./spatial_db --shell` can't start too few workers by accident.
- **The shell now uses `shard_<port>_wal.dat`, separate from the demo's `kitti_wal.dat`.** The demo has no shard map, so sharing a file could put points on the wrong worker. The cost is that data loaded in earlier shell sessions needs one more `LOAD`.
- **Verify before publishing.** `rebalance` compares the workers' total with the unique key count before it replaces the map.
- **No transaction around the whole rebalance.** A failed run leaves the old map, and running it again repairs the result. A true atomic version would be much larger.

### Verification

| Check | Result |
|---|---|
| Fresh 4 workers: load, rebalance, rebalance again | 28,846 each, then 0 moved on the second run, with counts unchanged at every step (115,384, 20,264, 4,962, 12,055) |
| Restart without the flag | Same 4 workers, data and boundaries. 8081 replayed 115,313 entries and restored 28,846 |
| `--shards 2` on 4-worker data | Refused with a clear message, exit 1 |
| Growing to 6 workers | 19,230 or 19,231 each, and it survived a restart |
| Invalid arguments | 6 of 6 refused |
| Fake worker dropping a transfer after 100 records | Reported as a failure, map unchanged, all points restored on the source |
| Rerun with 60 stray copies on the target | Repaired: 57,692 each, no duplicates, box count still 20,264 |
| Corrupt `shard_map.dat` | Rejected, map unchanged |
| Parser table | 48 of 48 passed |
| Line editing through a pseudo-terminal | 19 of 19 behaviours, and terminal settings restored in 4 of 4 ways of quitting |
| Demo | 115,384 ingested, 20,264 rows |
| TESTs 0-4 from `a20327a` with the two `5000.0f` changes | 18 of 18 passed, including `split_shard` on the new migration path |

### Known limits left in place

- More workers don't add speed, because the coordinator asks shards one at a time.
- Points already moved can be missed by queries between a failed rebalance and a successful one.
- A crash between the moves and saving the map has the same effect.
- Standbys aren't updated by a migration.

## 2026-11-05: Added line editing to the shell

Arrow keys printed `^[[C` and `^[[D` at the prompt, because `std::getline` has no line editing. The earlier tests all used piped input, so they couldn't catch this. These changes are uncommitted.

### Decisions

- **Wrote a small `LineEditor` rather than using `readline` or `libedit`.** The project has no dependencies, and linking a library would change the build on every machine. Asking users to install `rlwrap` would only hide the problem.
- **Added history and the usual shortcuts,** because the same key handling covers them. History is in memory only.
- **Raw mode lasts only while a line is being typed.** Commands run on a normal terminal, and a crash during a query can't leave the terminal raw.
- **Ctrl-C is handled as a key rather than a signal.** A signal would kill the process with the terminal still raw. It drops the current input, including a half-typed multi-line command.
- **Long commands scroll sideways.** The usual `FIND ... WHERE ... LIMIT` is longer than 80 columns, and redrawing with `\r` only reaches the current row, so wrapped lines would leave stale text.
- **Line editing only runs when stdin and stdout are both terminals,** so piped scripts behave as before.

### Verification

Run through a pseudo-terminal from scratch scripts: 19 of 19 behaviours passed, covering every listed key, unknown key sequences, a lone Esc, multi-line commands, and a long command at 40 columns. Terminal settings matched exactly after `EXIT;`, Ctrl-D, Ctrl-C and a session using arrows. Piped mode was unchanged.

### Known limits left in place

- Only ASCII can be typed.
- History isn't kept between sessions.

## 2026-11-05: Wrote ADR-001 for the query shell

Recorded the query shell design as [ADR-001](docs/adr/ADR-001-query-shell.md), covering why it uses a keyword language, client-side parsing and worker-side filtering, and which options were rejected. `decisions.md` stays the running log, while ADRs hold the full reasoning for larger design choices, so they live in `docs/adr/`.

## 2026-11-05: Added the query shell and worker-side filtering

People can now type commands at `./spatial_db --shell` instead of writing C++. `WHERE`, `LIMIT` and `COUNT` run on the worker, full scans are possible but explicit, and `intensity` is an alias for `metadata`. These changes are uncommitted.

### Decisions

- **Keyword syntax, not MongoDB-style JSON.** It's quicker to type for coordinates and needs a much smaller parser.
- **Every command ends with `;`.** The user asked for this. It also lets a command span several lines.
- **Skipped real timestamps and the `(zkey, timestamp)` index key.** The user left this out for now, so `SpatialRecord`, the WAL format and the index key are unchanged.
- **Parsing runs on the client side.** The coordinator holds the shard map and workers serve one connection at a time, so workers only needed one new command.
- **Added `CMD_QUERY_FILTERED` and left `CMD_QUERY` in place.** Existing callers keep working. `execute()` always uses the new path, so the old SDK tests cover it too.
- **`scan_box` takes a visitor that can stop early,** so `LIMIT` ends the scan rather than just trimming results. `range_query` wraps it, so there is one copy of the scan logic.
- **`LIMIT` asks each shard only for what's still missing,** which makes it exact without trimming afterwards.
- **`FIND ALL` requires `LIMIT`, and `COUNT ALL` doesn't.** A typo can't pull back the whole database, while counting returns one number.
- **`ALL` uses `±FLT_MAX` bounds,** so it also finds records `CMD_INGEST` clamped.
- **Logs are moved by redirecting `std::cout` to `spatial_shell.log`** instead of changing about 35 existing log lines.
- **Added `tests/`.** It's the first subfolder, because a test `main` can't sit with the sources that the build compiles together.

### Verification

| Check | Result |
|---|---|
| Lexer and parser table | 44 of 44 passed |
| Scripted shell session vs counts computed separately in Python | All matched: 115,384 all, 20,264 box, 4,962 box with `intensity > 100`, 12,312 for 50-100, 70 for `>= 200`, 12,055 for `= 0`, 91,721 with only an x range |
| `metadata` and `intensity` | Same result |
| Two shards split at the median key (57,693 + 57,691) | Counts add up, no duplicate rows, and `LIMIT` was exact at 1, 7, 57,000, 57,692, 60,000 and 115,384 |
| Malformed `filter_count` and `op` | Query fails and the worker keeps serving |
| Demo regression | 115,384 ingested, 20,264 rows |
| TESTs 0-4 from `a20327a` with the two `5000.0f` changes | 18 of 18 passed |

Fixed during testing: parse errors always said `line 2`, because a newline from the previous command was carried into the next.

### Known limits left in place

- The shell runs its own single worker, so it can't connect to a cluster that's already running.
- `WHERE` only compares `metadata`. It cuts network traffic but not scanning.
- Unquoted paths get a generic error, such as `LOAD 000000.bin;` giving `unexpected 'b' after number`.

## 2026-11-05: Updated the KITTI query block in main.cpp

The query block in [main.cpp](main.cpp) now uses the current SDK fields, so `main.cpp` compiles. The KITTI demo still can't build or run, because `KittiLoader.cpp` is empty and the blockers below remain. Nothing is committed.

### Decisions

- **Used `r.metadata` instead of the pasted `r.payload`.** `SpatialRecord` has no `payload` field, so the pasted line wouldn't compile. The printed label stays `Payload`.
- **Kept the existing bounding-box comment,** which the pasted block didn't include.
- **Fixed only the query block.** The blockers below are outside the requested edit, so they're reported rather than changed.

### Verification

`main.cpp` compiles on its own. Linking fails with an undefined `KittiLoader::load_bin_file`. The run results below came from a throwaway loader and a patched copy of `main.cpp` in the scratchpad, not from repo code.

### Blockers found

| Problem | Evidence |
|---|---|
| `KittiLoader.cpp` is empty (0 bytes). | The link fails. |
| `main.cpp` never calls `register_node`, and its shard range of `0..1e10` is far below real Morton keys. | All 300 test inserts aborted with no shard/endpoint. |
| One 2PC connection per point doesn't scale to a full frame. | With routing fixed, 95,449 of 115,384 inserts failed with `cannot reach Node 8081`, and the query then failed. The likely cause is running out of the 16,384 ephemeral ports, but `errno` wasn't captured. |
| The worker's `x < 0` vote rejects real data. | 52,237 of 115,384 points in `000000.bin` have `x < 0`, including 10,790 of the 20,264 inside the query box. |
| The block reports `[PASS]` with 0 rows, and `main` returns 0 after `[FAIL]`. | The unpatched run printed `[PASS]` with 0 points. The `[FAIL]` run exited 0 and would still print the closing `VERIFIED CLEAN` banner. |

### Follow-up: why KITTI ingestion reported 0 / 115384

`KittiLoader.cpp` has since been written and the shard range widened to the full `uint64_t` range. Every insert still aborted because `main.cpp` registers the shard but never calls `register_node`, so 8081 has no endpoint. This was analysis only, so `main.cpp` is unchanged.

Adding `register_node` in a scratch copy and running the real loader gave 17,234 / 115,384 ingested in 34 s, and the query then failed:

- 82,640 inserts failed with `cannot reach Node 8081`, starting at transaction 16,373. That matches the 16,384 ephemeral ports, and connected transactions (32,744) came to about two full port ranges, which fits ports freeing after the 30 s `TIME_WAIT`. This firms up the port-exhaustion explanation above, though `errno` still wasn't captured.
- 15,510 inserts were voted down by the `x < 0` rule.

### Follow-up: fixed KITTI ingestion

The full `000000.bin` frame now loads and queries correctly. These changes are uncommitted.

#### The error, in plain terms

There were three separate failures, and each one hid the next:

1. **Missing service registration (a configuration error).** The coordinator knew node 8081 owned the keys, but not its IP address and port.
2. **Ephemeral port exhaustion, caused by `TIME_WAIT`.** Every TCP connection needs a temporary local port, and macOS has 16,384 of them. A closed connection keeps its port for about 30 s. One connection per point used them all up, so `connect()` began to fail.
3. **Over-strict validation.** A leftover demo rule treated `x < 0` as invalid, but LiDAR x values are negative for points on one side of the sensor.

#### Decisions

- **Registered the node in `main.cpp`.** This is the direct fix for failure 1.
- **Added `MortonClient::insert_batch` and load 1,000 points per transaction.** A frame now takes 116 connections instead of 115,384. Reusing one connection would also have helped, but workers serve one connection at a time, so a long-lived client would block everyone else.
- **Replaced `x < 0` with `coord_in_range`.** The worker now rejects NaN, infinity and values the quantizer would clamp. These are coordinates the index can't store correctly, so the rule is real validation, and it still gives tests a way to force an abort.
- **Updated the rule's description in `CLAUDE.md` and `README.md`.**
- **Left the `[PASS]` on 0 rows and exit code 0 on `[FAIL]` alone,** because they weren't part of this request.

#### Verification

| Check | Result |
|---|---|
| Full frame | 115,384 / 115,384 ingested in about 3.1 s, up from 17,234 in 34 s |
| Bounding-box query | 20,264 rows, matching an independent count from the file; all 115,384 points fall in distinct millimetre cells, so none overwrite each other |
| Validation and batches (throwaway test) | 10 of 10 passed: negative x commits; 5000, -1001, NaN and infinity abort; a batch with one bad point stores nothing |
| TESTs 0-4 from `a20327a`, unmodified | 15 pass, 3 fail, all tied to `x = -1` being expected to abort |
| Same tests with those two `-1.0f` inputs changed to `5000.0f` | 18 of 18 pass, exit 0 |

Commit `6d4c44a` replaced those tests in `main.cpp`, so the repo's `main.cpp` no longer runs them. Their results above came from a scratch copy. If they are restored, the two inputs need the same change.

### Follow-up: brought CLAUDE.md up to date

`CLAUDE.md` still described the pre-KITTI project, and its build command no longer linked because it left out `KittiLoader.cpp`. It now covers the KITTI demo, `KittiLoader`, `insert_batch`, `coord_in_range`, the `register_node` requirement and the port limit. It also says how to run TESTs 0-4 from `a20327a`, because a future session would otherwise assume `main.cpp` still runs them. The build command was run as written, and the demo gave 115,384 ingested and 20,264 rows.

### Follow-up: rewrote README.md

The README described a much earlier version of the project, so it was rewritten against the code instead of patched. It said the build might fail, that the WAL had no recovery, and that negative coordinates were folded with `abs()`, none of which is still true. It also left out the SDK, optimizer, failover, shard splitting and KITTI loading. It now leads with the working build, documents the `TIME_WAIT` reason for `insert_batch`, and lists the current limitations. `CLAUDE.md` no longer warns that the README is stale.

### Follow-up: corrected the query box comment

The comments in `main.cpp` called the query box a forward zone, but KITTI's Velodyne axes are x forward, y left, z up, so y in [5, 25] is left of the car. The comments now say that. The box itself is unchanged, by choice, so results stay the same.

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
