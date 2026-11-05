# ADR-001: Typed query language with worker-side filtering

**Status:** Accepted (implemented, not yet committed)
**Date:** 2026-11-05
**Deciders:** AdityaAAND

## Context

The only way to read data was to write C++ against `MortonClient` and recompile. The project needed a way for people to type commands and fetch their data.

Four constraints shaped the decision:

- **No dependencies.** The project is plain C++17 with no build system, which rules out libraries and generator tools. This is also why the shell has its own small line editor rather than `readline`.
- **Workers serve one connection at a time.** A worker can't hold a long session for a user without blocking every other client.
- **Only the coordinator knows the shard map.** A worker can't answer for the whole cluster.
- **Results are large.** One 115,384-point KITTI frame is 2.77 MB of records, and a typical box query returns 20,264 rows (486 KB).

Timestamps were kept out of scope. Real timestamps need a new index key, which is a separate decision.

## Decision

Add a keyword query language, parsed on the client side, with filtering done on the workers.

```
FIND BOX x[-10,10] y[5,25] z[-2,3] WHERE intensity > 100 LIMIT 5;
COUNT ALL;
```

- **Language:** keyword commands, each ending in `;`, parsed by a hand-written lexer and recursive-descent parser.
- **Where parsing runs:** in the client process, which turns text into `MortonClient` calls.
- **Where filtering runs:** on the workers, through a new `CMD_QUERY_FILTERED` command that carries the box, up to 4 metadata filters, a limit and a count-only flag. `CMD_COUNT_RESP` returns a count.
- **Safety rule:** `FIND ALL` requires a `LIMIT`. `COUNT ALL` does not.

## Options Considered

### Option A: Keyword language, client-side parser, worker-side filtering (chosen)

| Dimension | Assessment |
|-----------|------------|
| Complexity | Medium: 655 lines of language code, 219 lines added across 10 existing files |
| Cost | One new wire command and one new reply |
| Scalability | Good: traffic scales with results rather than matches, and each worker filters its own shard |
| Team familiarity | High: follows the existing packed-struct protocol and SDK |

**Pros:**
- `COUNT` returns 8 bytes per shard instead of 486 KB of rows for the box above.
- `LIMIT` ends the scan early.
- The parser can be tested with no network.
- Only one new command reaches the workers, and they never see text.

**Cons:**
- The fixed-size request caps filters at 4 and supports `metadata` only.
- Adding a filterable field means changing a wire struct.

### Option B: MongoDB-style JSON queries

```
find({box: {x: [-10, 10], y: [5, 25]}, metadata: {$gt: 100}, limit: 5})
```

| Dimension | Assessment |
|-----------|------------|
| Complexity | Higher: needs a nested parser, with no JSON library available |
| Cost | Same wire changes as Option A |
| Scalability | Same as Option A |
| Team familiarity | Familiar style for NoSQL users |

**Pros:** looks like other NoSQL databases, and nested documents extend naturally to new query shapes.

**Cons:** slower to type for coordinate ranges, and this data has fixed fields, so the flexibility would go unused.

### Option C: Keyword language with filtering in the client

| Dimension | Assessment |
|-----------|------------|
| Complexity | Lowest: no protocol or worker changes |
| Cost | Every matching row crosses the network, even for `COUNT` |
| Scalability | Poor: traffic grows with matches, whatever the query asks for |
| Team familiarity | High |

**Pros:** smallest change and no new wire format.

**Cons:** `COUNT` and `LIMIT` would look cheap but still transfer every match. `COUNT ALL` would move the full 2.77 MB to return one number.

### Option D: Workers accept text commands directly

| Dimension | Assessment |
|-----------|------------|
| Complexity | High: workers would need a parser and a route to other shards |
| Cost | A second protocol beside the binary one |
| Scalability | Poor: interactive sessions would hold a worker's only connection |
| Team familiarity | Low: breaks the coordinator's role |

**Pros:** clients could connect with a simple tool such as `nc`.

**Cons:** a worker doesn't know the shard map, so it can't answer for the cluster, and a user session would block all other clients of that worker.

## Trade-off Analysis

**A over C is the main call.** C would have been faster to build, but the operations users expect to be cheap would have been the expensive ones. A moves work to where the data lives, at the cost of a wider wire protocol. Keeping `CMD_QUERY` in place limited the risk, and the old cluster tests still pass 18 of 18.

**A over B favours typing speed over familiarity.** The data has a fixed shape, so document-style flexibility has little value here. A keyword grammar also keeps parser errors simple, down to a line and column.

**Hand-written parser over flex/bison.** The grammar is small, and generators would add a tool to a project with no dependencies. The cost is that a much larger grammar would be harder to maintain by hand.

**Fixed-size filters over a variable-length payload.** Every existing payload is one packed struct read by `sizeof`, so this keeps the worker's read path simple and easy to validate. The cost is the hard limit of 4 filters.

**Required `LIMIT` on `FIND ALL`.** This gives up convenience so that one typo can't request the whole database in a single response.

## Consequences

**Easier:**
- Anyone can query without compiling C++.
- Commands can be scripted, because the shell reads from stdin.
- Queries can be checked against the optimizer with `EXPLAIN`.
- SDK users get `where`, `limit` and `execute_count` too.

**Harder:**
- There are now two query commands to keep working, `CMD_QUERY` and `CMD_QUERY_FILTERED`.
- A new filterable field touches the protocol, worker and parser.
- `spatial_shell.log` hides logs from the terminal, so debugging means opening that file.

**To revisit:**
- **Remote connections:** the shell runs its own workers in one process and can't join a running cluster. This is the biggest gap.
- **Scan cost:** `WHERE` cuts traffic, not scanning, because workers still walk every key between the box corners.
- **Result size:** a `FIND` with no `LIMIT` still transfers every match. Very large results would need paging.
- **Timestamps:** a time filter depends on the separate index key decision.

## Verification

| Check | Result |
|---|---|
| Lexer and parser table | 44 of 44 passed |
| Shell counts vs an independent Python count | All matched |
| Two shards, split at the median key | Counts added up, no duplicates, `LIMIT` exact at all 6 sizes tried |
| Malformed filter request | Query failed and the worker kept serving |
| Existing cluster tests, with two inputs updated | 18 of 18 passed |

Interactive line editing was tested through a pseudo-terminal, not by hand in every terminal app. See the 2026-11-05 entry on line editing in `decisions.md`.

## Action Items

1. [x] Add line editing, after arrow keys printed `^[[C` and `^[[D` at the live prompt.
2. [ ] Commit the work, and decide whether the unfinished `SplitShardPayload` edit goes with it.
3. [ ] Bring the cluster tests back into the repo, because they exist only in commit `a20327a`.
4. [ ] Improve the error for unquoted paths such as `LOAD 000000.bin;`.
5. [ ] Decide how the shell should connect to a running cluster.
6. [ ] Write a separate ADR for timestamps and the index key.
