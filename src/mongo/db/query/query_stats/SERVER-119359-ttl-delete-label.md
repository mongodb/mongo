# SERVER-119359 — Distinguish TTL deletes from user deletes in QueryStats

Status: design, w3-92 (branch `substrate-contrib/w3-92`). Epic SPM-4328.
Team: Query Integration. Reporter: Arun Banala.

## Problem

`queryStats` collects per-shape metrics for `find`/`aggregate`/`update`/`insert`
but cannot answer "which deletes are driven by the TTL monitor versus a user
connection?" Today there is no `DeleteKey` class and no per-key origin label —
operators reading `$queryStats` cannot separate housekeeping pressure from
client traffic when sizing the cache or chasing a regression.

## Annotation site

The TTL monitor enters delete execution at exactly two points in
`src/mongo/db/ttl/ttl_monitor.cpp`:

- `TTLMonitor::_deleteExpiredWithIndex` (line ~564) — indexed TTL collections,
  constructs `DeleteStageParams` + `CanonicalQuery` then calls
  `InternalPlanner::deleteWithIndexScan` → `exec->executeDelete()`.
- `TTLMonitor::_performDeleteExpiredWithCollscan` (line ~759) — clustered /
  collscan path, used by `_deleteExpiredWithCollscan` and the timeseries
  extended-range variant.

Both paths converge on a single `OperationContext*` owned by the TTL monitor
thread. The annotation is a one-line decoration on that `OperationContext`
*before* the delete executor runs — every downstream QueryStats key derived
from this op-context inherits the label.

Proposed annotation: a small `OperationContext` decoration
`TTLDeleteOriginDecoration` (bool, default `false`) flipped to `true` inside
both `_deleteExpiredWithIndex` and `_performDeleteExpiredWithCollscan` before
executor construction. Lives in `src/mongo/db/query/query_stats/` so the
QueryStats layer owns the surface; TTL monitor only writes it.

## QueryStats key surface

A new `DeleteKey` (peer of `UpdateKey` / `InsertKey`) is required — registered
from the existing delete dispatch in `write_ops_exec.cpp` behind a feature flag
`featureFlagQueryStatsDeleteCommand`, mirroring the update wiring at
`computeShapeAndRegisterQueryStats` (line 507) and
`computeInsertShapeAndRegisterQueryStats` (line 567).

`DeleteCmdComponents` (peer of `UpdateCmdComponents`) carries:
- `_ordered` (existing pattern)
- `_bypassDocumentValidation`
- `_origin` enum `{kClient, kTTL, kInternal}` — read from the
  `TTLDeleteOriginDecoration` at key-construction time. The TTL monitor never
  routes through user-facing CRUD, but the decoration is the single
  authoritative source.

`appendTo()` emits `origin: "ttl"|"client"|"internal"` in the serialized key
output. Hashing includes `_origin` so TTL deletes and user deletes against the
same shape land in different buckets — operators reading
`$queryStats({transformIdentifiers: ...})` can group by `key.origin` directly.

`static_assert` on `sizeof(DeleteKey) == sizeof(Key) + sizeof(DeleteCmdComponents)`
follows the `UpdateKey` precedent (update_key.h:106).

## Why a key-level label, not a metric

A supplemental metric (à la `nUpdateOps`) would collapse TTL and user deletes
onto the same shape entry — exactly the conflation the ticket asks us to undo.
Promoting origin into the key gives operators independent rows, independent
LRU pressure, and independent rate-limiting.

## Test surface

Pinned by `jstests/noPassthrough/query/queryStats/query_stats_ttl_delete_origin.js`
(this changeset). The jstest:

1. Starts a single-node replica set with `ttlMonitorSleepSecs: 1`, slow-ms 0.
2. Creates a TTL index `{expireAt: 1}` with `expireAfterSeconds: 0`.
3. Inserts a doc with `expireAt` in the past, waits for one TTL pass.
4. Issues a peer `deleteOne({ _id: ... })` from the user shell against a fresh
   non-expired doc with the same shape.
5. Reads `$queryStats` and asserts two entries exist for `command: "delete"`
   on the same collection: one with `key.origin == "ttl"`, one with
   `key.origin == "client"`. Asserts neither row reports the other origin's
   count under `metrics.execCount.sum`.

Failure modes the test pins:
- Annotation not propagated → both rows collapse to `client` → assertion fires.
- Annotation leaks across ops → user delete shows `ttl` → assertion fires.
- DeleteKey not registered → `getQueryStats` returns no `delete` rows →
  `getLatestQueryStatsEntry` throws on `assert.neq([], sortedEntries)`.

## Out of scope (follow-ups)

- Resharding / migration / chunk-cleanup deletes — same annotation hook can
  carry `kInternal`, but those callers are not in this change.
- mongos-side aggregation of `origin` across shards — covered by the existing
  `query_stats_update_cmd_metrics_mongos.js` pattern; deferred to a sibling
  ticket.
- Feature-flag rollout / FCV gating — follows the
  `gFeatureFlagQueryStatsUpdateCommand` precedent (write_ops_exec.cpp:452).
