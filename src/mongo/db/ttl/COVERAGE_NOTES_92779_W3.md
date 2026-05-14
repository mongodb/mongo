# SERVER-92779 — Wave-3 Extended Coverage Notes

**Status**: companion to `jstests/sharding/ttl_orphans_multishard_coverage.js`.
**Umbrella**: SERVER-92779 — *TTL delete progress blocked by unowned documents*.
**Wave-1**: SERVER-126541 (commit `d65fe1832d`) — single-shard repro at
`jstests/sharding/ttl_blocked_by_unowned_docs.js` + proposed BatchedDeleteStage
accounting fix at `src/mongo/db/ttl/PROPOSED_FIX.md`.

## Why wave-3 exists

Wave-1 reproduced the BatchedDeleteStage accounting bug on a single donor
shard. That is sufficient to land the two-line fix Haley Connelly outlined
in the original ticket steps-to-reproduce, but it leaves two
behaviour-coverage gaps that the umbrella ticket implicitly assumes:

1. **Multi-shard parallelism.** SERVER-92779's user-visible symptom in
   Atlas tickets shows up on chunk-rebalanced collections where multiple
   donor shards each hold orphan bands. If a fix only un-sticks the
   single-donor case (e.g. by special-casing a per-shard predicate), it
   may still leave 2-of-3 donors stuck when chunks are rebalanced
   between three or more shards. Wave-3 Variant A exercises this — it
   places independent orphan bands on shard0 AND shard2 simultaneously
   and asserts the owned canary on shard1 still gets deleted.

2. **Cross-namespace TTL pass scheduling.** `TTLMonitor::doTTLSubPass`
   walks every registered TTL index in one sub-pass via
   `getTTLSubPasses` (`ttl.cpp`). The wave-1 fix touches
   `BatchedDeleteStage` (per-stage) plus the per-collection
   reschedule branch in `_deleteExpiredWithIndex`. A subtle failure
   mode is: collection A saturates `targetPassDocs` with orphans on a
   sub-pass, the monitor reschedules collection A — but does collection
   B on the same shard get its turn before A's reschedule re-saturates?
   Wave-3 Variant B pins that contract: collection B's expired-owned
   docs MUST be deleted within the soak window even when collection A
   is in the stuck state.

## Variants

### Variant A — multi-shard fan-out (3 shards)

Topology after setup:
- shard0: orphan band A (ids `[-2N, -N)`, 150 docs, expired).
- shard1: single owned canary (id 0, expired).
- shard2: orphan band C (ids `[N, 2N)`, 150 docs, expired).

`N = kOrphanBand = 150 > ttlIndexDeleteTargetDocs = 100`.

Pre-fix expectation (FIX_LANDED=false): shard0 and shard2 are both stuck
on their orphan bands; the owned canary on shard1 may or may not expire
depending on the monitor's scheduling cadence between shards, but the
two donor shards remain saturated forever.

Post-fix expectation: the canary expires within 60s and the orphan
bands remain (range deleter disabled — orphans are SERVER-92779's
precondition, not its target).

### Variant B — cross-namespace coverage (2 collections, 1 donor shard)

Topology after setup (on shard0):
- `coll_with_orphans`: orphan band (150 expired orphans) + 1 owned
  expired doc.
- `coll_clean`: 25 expired owned docs, no orphans.

Both have TTL indexes on `ttlField` with `expireAfterSeconds: 1`.

Pre-fix expectation: `coll_with_orphans` saturates `targetPassDocs`
every pass; whether `coll_clean` makes progress depends on the
scheduling between TTL indexes — a naive per-collection fix that
reschedules `coll_with_orphans` repeatedly without yielding to
`coll_clean` would leave the latter starved.

Post-fix expectation: both collections drain their owned-expired
documents within the soak window.

## Tag gating

Both variants are tagged `__TEMPORARILY_DISABLED_PENDING_SERVER_92779`,
matching wave-1's gating convention. The `FIX_LANDED` constant at the
top of the jstest is the single switch. Flip it to `true` (and remove
the tag) in the commit that lands the BatchedDeleteStage accounting fix
+ the cross-namespace reschedule guard.

## What this file is NOT

- Not a proposed fix. The fix proposal lives in wave-1's
  `src/mongo/db/ttl/PROPOSED_FIX.md` and the QE-side discussion on
  WRITING-26564 (resumable TTL deletes).
- Not a substitute for the receiver-side variant noted at the bottom
  of the SERVER-92779 description ("Expired orphan documents on a
  recipient shard, which belong to a chunk that has yet to be
  committed"). That deserves its own wave — the migration commit
  ordering is more delicate than the donor-side repro and the council
  flagged it as a separate harness.
- Not a passthrough or fsm workload. SERVER-92779 is fundamentally
  about the interaction between TTLMonitor, BatchedDeleteStage, and
  the shard-filtering predicate; a dedicated ShardingTest is the right
  shape, not a passthrough.

## Cross-references

- Umbrella: SERVER-92779.
- Wave-1: SERVER-126541, commit `d65fe1832d`,
  `jstests/sharding/ttl_blocked_by_unowned_docs.js`,
  `src/mongo/db/ttl/PROPOSED_FIX.md`.
- Existing related coverage:
  `jstests/sharding/ttl_deletes_not_targeting_orphaned_documents.js`
  (asserts TTLMonitor doesn't delete orphans — but never exercises the
  stuck-progress bug because its owned chunk is moved entirely off the
  donor shard).
- Source touchpoints:
  `src/mongo/db/exec/batched_delete_stage.cpp:389-394` (orphan skip),
  `:516` (pass-total increment),
  `src/mongo/db/ttl/ttl.cpp:96` (`targetPassDocs` set from
  `TTLIndexDeleteTargetDocs`),
  `src/mongo/db/ttl/ttl.idl:91` (server parameter declaration).
