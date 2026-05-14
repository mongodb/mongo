# Query Stats — Insert Command on Sharded Clusters

This note extends [`README.md`](README.md) (Metric Collection / Adding New Metrics) with the
design for query-stats collection of the `insert` command on sharded clusters. It captures the
work tracked in [SERVER-122076][ticket], the follow-up to [SERVER-122054][parent] (which wired
query stats for inserts on standalone / replica-set deployments).

The entire feature stays guarded by `featureFlagQueryStatsInsert` (default off).

## Background

`SERVER-122054` registers an `InsertCmdShape` and aggregates per-insert metrics on `mongod` via:

1. `write_ops_exec.cpp::performInserts` → `computeInsertShapeAndRegisterQueryStats` (once per
   command, pre-batch).
2. `performInserts` → `collectQueryStatsMongod` (once per command, post-batch).

`execCount` bumps **per command dispatched** (find-style semantics), not per statement, matching
the architectural rule that an `insert` command is one logical invocation that may carry many
documents and survive partial retryable-write replays.

The standalone PR explicitly leaves the mongos/sharded write path disabled — see the
`describe.skip("query stats insert command metrics (sharded)")` block in
`jstests/noPassthrough/query/queryStats/query_stats_insert_cmd_metrics.js`.

## Goals (SERVER-122076)

1. An `insert` command that enters the cluster through `mongos` produces **exactly one**
   `$queryStats` entry on the router, regardless of whether the underlying batch fans out
   to one or many shards.
2. `execCount` on the router increments once per top-level `insert` command, mirroring the
   standalone semantics. Per-document accounting still flows through `nInserted` aggregates.
3. The query shape recorded on the router is identical (by `queryShapeHash`) to the shape
   recorded on every shard that participated in the write — so `$queryStats` aggregation
   across the cluster collapses to one logical entry per shape.
4. Internal retries triggered by `StaleConfig`, `MigrationConflict`, `ShardCannotRefreshDueToLocksHeld`,
   etc., **do not** double-count: the router records one execution per externally-issued
   command, even if mongos re-dispatched the batch internally.
5. Encrypted inserts (`wholeOp.getEncryptionInformation()`) and inserts from internal /
   direct clients continue to bypass shape and stats recording, exactly as in standalone.

## Non-goals

- `bulkWrite` insert ops — tracked separately under the bulkWrite query-stats stack.
- Time-series inserts on sharded clusters that go through the viewless/bucket rewrites — the
  shape/key path already handles `kTimeseries` in the standalone PR; sharded targeting of the
  bucket collection is verified by the new jstest but no new shape work is required.
- Retryable-write idempotency on the shard side (already pinned by SERVER-122054 unit tests).

## Design

### Router hook site

The mongos insert path runs through the cluster-write command in `src/mongo/s/commands/`
(cluster_write_cmd / cluster_bulk_write). The pattern mirrors the update wiring:

1. **Pre-dispatch**: at the top of the cluster `insert` command's `_runImpl` (or equivalent),
   call `computeInsertShapeAndRegisterQueryStats(opCtx, request)`. The OperationContext-only
   overload of `computeQueryShapeHash` added in SERVER-122054 already covers this — no
   ExpressionContext is available here, and the IDHACK / FLE short-circuits are not relevant
   on the router because targeting has not yet run.
2. **Post-dispatch (success or partial failure)**: after the batch has been dispatched and the
   reply assembled, call `collectQueryStatsMongos(opCtx, ...)` to materialize the
   `QueryStatsEntry` on the router's `QueryStatsStore`. This is the same site at which the
   update path calls into `collectQueryStatsMongos` today.
3. **Idempotency under retry**: `StaleConfig` retries inside mongos must not bump `execCount`.
   The hook lives outside the retry loop in the cluster command's outer frame — exactly the
   shape used by the update wiring tested in `query_stats_update_cmd_metrics_mongos.js`
   ("StaleConfig retried update" describe block).

### Shape consistency between router and shards

`InsertCmdShape` is constructed purely from `(nss, collectionType, command='insert')` plus
the shape of the documents array (after FLE / identifier-transform). Because neither the
router nor the shard rewrites these fields between the cluster command and the per-shard
`performInserts` call, the `queryShapeHash` is identical on both sides — verified by the
new jstest asserting equality of `entries[0].key.queryShape` between `mongos` and each
participating `shard`.

### Encryption / internal-client bypass

`InsertCmdShape` registration is skipped on the router whenever:

- `wholeOp.getEncryptionInformation()` is set (matches mongod), OR
- the caller is an internal client (existing `Client::isInternalClient()` check on the
  cluster-command frame), OR
- the namespace is in the always-skip allowlist (`$queryStats` introspection itself,
  `local.*`, etc.).

The unsharded case on a mongos-fronted collection still goes through the same hook — the
router records exactly one entry even though all data lives on the primary shard.

## Test plan

| File | Purpose |
| --- | --- |
| `jstests/sharding/queryStats/query_stats_insert_cmd_sharded.js` (this PR) | Per-deployment metric checks: single-doc / multi-doc insert; unsharded collection on a sharded cluster; sharded collection with single-shard-target batch; sharded collection with multi-shard fan-out; retryable-write across mongos; StaleConfig retry idempotency; cluster vs shard shape-hash equality. |
| `query_stats_insert_command_feature_flag.js` (existing) | Feature flag is present and off-by-default — covers mongos automatically because the flag is a single bool. |
| `insert_cmd_mongod_slow_query_log.js` (existing) | mongod slow-query log carries `queryShapeHash`. The mongos counterpart for `insert` is filed as a follow-up under SERVER-122076 once the slow-log path is wired; not in scope for this jstest. |

Shape-equality and `execCount` invariants are the load-bearing assertions; metric sums
(`nInserted`, `keysExamined`, `docsExamined`) follow the standalone PR's accounting and are
asserted via `assertAggregatedMetricsSingleExec`.

## References

- Parent ticket: [SERVER-122054][parent] — standalone / replica-set wiring (merged, then
  reverted in `58feea5679`; rolling forward).
- This ticket: [SERVER-122076][ticket] — sharded write-path wiring.
- Epic: [SPM-3697][epic].
- Sibling design: `query_stats_update_cmd_metrics_mongos.js` is the closest pattern for the
  mongos hook — same StaleConfig / two-phase / retryable-write surfaces apply.

[ticket]: https://jira.mongodb.org/browse/SERVER-122076
[parent]: https://jira.mongodb.org/browse/SERVER-122054
[epic]: https://jira.mongodb.org/browse/SPM-3697
