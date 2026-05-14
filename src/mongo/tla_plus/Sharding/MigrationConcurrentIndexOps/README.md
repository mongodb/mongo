# MigrationConcurrentIndexOps

TLA+ model of the race between chunk migration and the `createIndexes` / `dropIndexes`
commands on a sharded collection. The race is documented in
[SERVER-99357](https://jira.mongodb.org/browse/SERVER-99357): a user-issued index command
fans out shard-by-shard via the router's retry loop, while a migration recipient takes a
single index-catalog snapshot from the donor at the start of the clone phase. If the index
command interleaves between the recipient's snapshot and the donor's per-shard step, the
cluster ends with different index sets on different shards after the migration commits.

The downstream effect is that future migrations refuse to copy data into a shard whose
existing index set diverges from the donor's, which blocks `removeShard` and
`transitionToDedicatedConfigServer` until the operator manually reconciles indexes.

## What the spec models

- A single chunk-bearing collection on a set of shards (cardinality >= 2; the third shard
  is a bystander to force per-shard ordering to matter).
- Migration phases: `Unset -> Cloning -> RecipientPrepared -> AllPrepared -> Committed
  -> Unset`, with an abort path back to `Unset`.
- The recipient's clone-time index snapshot, captured in `migration.snapshot` at
  `MigrateStartClone` and applied via `MigrateRecipientApplyClone`.
- Router-driven index ops as ordered fan-outs over data-bearing shards. Each `StepIndexOp`
  mutates one shard's local index set; the order is non-deterministic, modelling the
  shard-version retry loop's lack of cross-shard atomicity.
- A bug toggle, `SafeIndexSyncOnCommit`. When `TRUE` the recipient re-clones the donor's
  current index catalog at commit (the fix); when `FALSE` the recipient keeps its stale
  clone-time snapshot (the bug).

## Invariants

- `IndexSetConsistentPostMigration` -- the headline invariant. When no migration and no
  index op is in flight, every data-bearing shard has the same index set.
- `MigrationPhaseWellFormed` -- shape check on the migration record.
- `TypeOK` -- standard type invariant.

## Running

```
cd src/mongo/tla_plus
./download-tlc.sh        # one-time, downloads tla2tools.jar
./model-check.sh Sharding/MigrationConcurrentIndexOps
```

`MCMigrationConcurrentIndexOps.cfg` is the green config (`SafeIndexSyncOnCommit = TRUE`)
and is expected to discharge with no invariant violations. To witness the bug, swap in
`MCMigrationConcurrentIndexOps_bug.cfg` (rename it over the green cfg or pass it via
`-config` if invoking TLC directly); TLC then dumps a counter-example trace of the form
documented inline in the bug cfg.

## Reproducer jstest

The deterministic, single-process repro lives at
`jstests/sharding/migration_concurrent_index_ops_deterministic.js`. It uses migration
failpoints to wedge the recipient at clone time, runs a `dropIndexes` through the router,
unwedges the migration, and then asserts that the per-shard index sets diverge before the
fix and stay consistent after. The TLA+ spec and the jstest exercise the same trace shape;
the spec proves the property holds under the fix on the full state space, the jstest pins
the bug shape against accidental regression.
