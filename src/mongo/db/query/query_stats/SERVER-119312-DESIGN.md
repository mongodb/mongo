# SERVER-119312: Redefining `lastExecutionMicros` and `totalExecMicros` for router-side writes

Status: design proposal, scoped to query stats observability semantics.
Priority: Critical - P2.
Epic: SPM-4598.
Team: Query Integration.

## 1. Problem statement

The query stats store exposes two timing fields per query shape:

- `lastExecutionMicros` — last execution time, in microseconds.
- `totalExecMicros` — `AggregatedMetric<uint64_t>` (sum / max / min / sumOfSquares) of
  execution time, in microseconds, across every execution of the shape.

For queries (find, agg, count, distinct, etc.) these fields have a clean operational
meaning: a query is executed exactly once per request, the timer is the per-op
`AdditiveMetrics::executionTime` set in `CurOp::finishCurOp` (which already excludes
pauses via `CurOp::elapsedTimeExcludingPauses()`), and the captured snapshot is
written to `lastExecutionMicros` / aggregated into `totalExecMicros` via
`updateStatistics` in `src/mongo/db/query/query_stats/query_stats.cpp`.

For **writes on a router**, the same fields are misleading along two independent
axes:

1. **No per-op time exists on the router.** A single `update` / `delete` / `insert`
   client command can carry multiple ops in `updates[]` / `deletes[]` /
   `documents[]`. The router holds one `OpDebug::AdditiveMetrics::executionTime`
   for the whole command — it does **not** time each op individually. The
   per-op-index `QueryStatsInfo` slot in
   `OpDebug::_queryStatsInfoForBatchWrites` carries its own
   `AdditiveMetrics`, but `executionTime` on that per-op slot is currently set
   from `CurOp::elapsedTimeExcludingPauses()` at end-of-op (see
   `CurOp::setEndOfOpMetricsForBatchWrites` in `src/mongo/db/curop.cpp`) — i.e.
   every op in the batch is stamped with the *whole-command* elapsed time. As
   a result, `lastExecutionMicros` for a shape that fired N times in a batch
   currently reflects N copies of the same whole-command duration, and
   `totalExecMicros.sum` for a single client batch grows roughly linearly in the
   number of ops in that batch, with each summand being the *batch* duration,
   not the op duration. See README.md row 169–171 for the existing
   not-quite-honest description.

2. **"Execution time" subtracts pauses.** `CurOp::elapsedTimeExcludingPauses()` deducts
   intervals during which the op was explicitly paused (e.g. yield points). On the
   router, the user-perceived dimension is **wall latency** — the interval from
   the moment the router receives the batched request to the moment it sends the
   response, *including* any pauses (network round-trips to shards, retries on
   StaleConfig, sub-transaction handling for WouldChangeOwningShard, etc.).
   Pause-subtraction makes a router-side execution-time number that is neither
   the user-perceived latency *nor* the actual shard execution time.

The combined effect: on the router, `lastExecutionMicros` and `totalExecMicros`
for a write shape are **simultaneously misleading on naming, on aggregation
boundary, and on definition.**

## 2. Surface area

In code (read-only):

- `src/mongo/db/query/query_stats/query_stats_entry.h:229,239` — field declarations.
- `src/mongo/db/query/query_stats/query_stats_entry.cpp:48,50` — `toBSON` emit names.
- `src/mongo/db/query/query_stats/query_stats.cpp:342,346` — `updateStatistics`
  writes both fields from `QueryStatsSnapshot::queryExecMicros`.
- `src/mongo/db/query/query_stats/query_stats.h:239` — `queryExecMicros` snapshot
  field; sourced from `AdditiveMetrics::executionTime` in `captureMetrics`
  (`query_stats.cpp:554`).
- `src/mongo/db/curop.cpp:587` — `setEndOfOpMetricsForBatchWrites` stamps
  per-op-index slots in `OpDebug::_queryStatsInfoForBatchWrites` from
  `elapsedTimeExcludingPauses`.
- `src/mongo/db/op_debug.h:575–649` — per-op-index `QueryStatsInfo` plumbing for
  batched writes on the router.
- `src/mongo/db/commands/query_cmd/write_commands.cpp:567–587` — router collects
  `write_ops::QueryStatsMetrics` (per-op-index + per-op `CursorMetrics`) from
  shards and surfaces them in the update/delete/insert reply.

In observability surfaces:

- `$queryStats` aggregation stage output. Today emits raw
  `lastExecutionMicros` / `totalExecMicros` for write shapes with no
  distinguishing flag.
- README.md row 169–172 already notes "for writes, sum of execution time of all
  ops in client batch" — i.e. the misleading aggregation boundary is documented
  but not corrected.

## 3. Proposed metric semantics

Adopt the framing already suggested in the Jira description: include **both**
a latency view (no pause subtraction, router-bounded) and an
execution-time view (pause-subtracted, op-bounded if available, else marked as
batch-bounded). Surface this only for shapes whose first-seen command was a
write driven by a router; do not change query (find/agg/count/distinct)
semantics.

### 3.1 Field set

For router-side writes a query stats entry's `metrics` document will
additionally include:

| Field                       | Type                          | Definition |
|-----------------------------|-------------------------------|------------|
| `routerLatencyMicros`       | `AggregatedMetric<uint64_t>`  | Wall-clock interval from router receiving the batched command to router sending the response. Does NOT subtract pauses. One sample per client batch (not per op in the batch). |
| `lastRouterLatencyMicros`   | `uint64_t`                    | The latest `routerLatencyMicros` sample. |
| `routerLatencySource`       | string (`"clientBatch"`)      | Documents the aggregation boundary — one sample per client batched command, **not** per op-index. |
| `executionTimeBoundary`     | string (`"clientBatch"` / `"op"`) | Documents the boundary the existing `lastExecutionMicros` / `totalExecMicros` were captured over. Today on the router for writes this is `"clientBatch"`. When SERVER-121325 lands and per-op timing exists, this flips to `"op"` and the existing field semantics become correct without renaming. |

The existing `lastExecutionMicros` and `totalExecMicros` keep their names but
their **meaning** is now disambiguated by `executionTimeBoundary`. They
continue to record what they record today — that is, the
`elapsedTimeExcludingPauses` value the per-op-index slot was stamped with
in `setEndOfOpMetricsForBatchWrites` — but consumers no longer have to guess
whether the duration is op-bounded or batch-bounded.

### 3.2 Where the latency sample comes from

The router-side wall latency for a batched write is the duration between
`OpDebug` start (when the command enters the router) and the moment the router
finalizes its reply. This is the *same* `elapsedTimeTotal` that already exists
on `CurOp` (vs. `elapsedTimeExcludingPauses`); we sample it once per client
batch, attached to the per-op-index slot of the *first* op-index in the batch
(i.e. the slot that drives the query stats entry for this shape today).

Aggregation strategy: `AggregatedMetric<uint64_t>::aggregate` is called once per
client batch — not once per op in the batch — so that `routerLatencyMicros.sum`
across N client batches equals the sum of N wall-latency samples, and
`routerLatencyMicros.max` is the worst observed batch latency. This is the
natural denominator for "p99 router-side write latency for this shape."

### 3.3 Why not just rename / redefine in place

Renaming `lastExecutionMicros` and `totalExecMicros` would be a breaking change
for every dashboard, BIC pipeline, and Atlas surface consuming `$queryStats`
today. The proposal preserves the names but adds an honest companion field plus
boundary tags. This is the pattern README.md already uses for cursor metrics
("Some metrics computed on the router are sourced differently from those on
shards" — row 160).

### 3.4 What the consumer sees

For a query shape that has only ever been seen on a router as part of a write,
`$queryStats` will return:

```json
{
  "key": {...},
  "metrics": {
    "execCount": 17,
    "lastExecutionMicros": NumberLong(842),
    "totalExecMicros": { "sum": NumberLong(15410), "max": NumberLong(2103), ... },
    "executionTimeBoundary": "clientBatch",
    "routerLatencyMicros": { "sum": NumberLong(28800), "max": NumberLong(4801), ... },
    "lastRouterLatencyMicros": NumberLong(2901),
    "routerLatencySource": "clientBatch",
    ...
  }
}
```

The boundary tag makes the existing fields' provenance machine-readable;
`routerLatencyMicros` gives consumers the wall-latency view they actually want
for write SLOs.

## 4. What we are NOT solving here

- We are not introducing per-op timing on the router. That belongs to SERVER-121325
  (and the related family of "double-count" / "WouldChangeOwningShard metrics
  not propagated" bugs). When that lands, the only diff to this design is
  flipping `executionTimeBoundary: "clientBatch"` → `"op"` for write shapes,
  which is a one-line change in `updateStatistics`.

- We are not changing query-side (find/agg/count/distinct) semantics.
  Those shapes never go through `_queryStatsInfoForBatchWrites`; the new
  fields are emitted only when the entry's first-seen command was a router-side
  write, gated by `includeWriteMetrics` in `QueryStatsEntry::toBSON`.

- We are not addressing `firstResponseExecMicros`. That field is cursor-shaped
  ("time to first batch") and has no analog for writes; today it is emitted as
  zero for write shapes and the README documents it as such.

## 5. Test plan

A new jstest, `jstests/noPassthrough/query/queryStats/query_stats_write_latency_vs_exec_router.js`,
pins three properties end-to-end on a `ShardingTest`:

1. **Boundary tag is present and consistent.** Every `$queryStats` entry whose
   key encodes an update/delete/insert command, surfaced via mongos, carries
   `executionTimeBoundary` and `routerLatencySource` strings.

2. **`routerLatencyMicros` ≥ `totalExecMicros` per-batch.** For a single client
   command, the wall latency must equal or exceed the pause-subtracted exec time
   (latency is the bigger bowl). We assert this on `.max` field-to-field, since
   `.max` is the only `AggregatedMetric` field that is comparable per-shape
   without a per-batch labeling channel.

3. **Aggregation boundary differs.** When a single client batch contains
   multiple ops of the same shape (e.g. two `updateOne`s with identical query
   shape after normalization), `totalExecMicros.sum` is consistent with the
   number of ops in the batch (i.e. multiple summands; the existing
   batch-stamped behaviour), while `routerLatencyMicros.sum` increments by
   exactly one sample per *batch*. Asserted via `execCount` deltas and
   counts of distinct AggregatedMetric samples (sum/count ratio).

The jstest exercises the targeted-single-shard fan-out path (the common router
write path) to keep wall time predictable and avoids the StaleConfig / WCOS
paths that today have their own TODO-tracked metric gaps.

## 6. Rollout

- Phase 1 (this design + jstest): doc the corrected semantics, pin the
  invariants in a jstest. The jstest skips its boundary assertions when
  the new fields are absent so it stays green on builds that do not yet ship
  the implementation.
- Phase 2 (separate ticket): plumb `routerLatencyMicros` capture and
  `executionTimeBoundary` tagging through `QueryStatsSnapshot`,
  `updateStatistics`, and `QueryStatsEntry::toBSON`. Gate behind a new
  feature flag `featureFlagQueryStatsRouterWriteLatency`.
- Phase 3: drop the skip in the jstest, flip the feature flag on by default,
  update the README.md table row 169–172 to refer to the boundary tag.

## 7. Open questions

- Should `executionTimeBoundary` also be emitted for query (non-write) shapes
  as `"op"` for symmetry / forward-compat? Default is no — only emit for
  writes, since the absence of the tag on query shapes is itself the contract
  ("query shapes have always been op-bounded"). Reversible later.

- Aggregation boundary for ordered vs. unordered batched writes: today both
  share the per-op-index slot machinery (`_queryStatsInfoForBatchWrites`); the
  proposal aggregates `routerLatencyMicros` once per client batch in both
  cases. If ordered batches grow short-circuit semantics that cause the router
  to send back early, the latency sample remains correct (it is whatever the
  router observed end-to-end).
