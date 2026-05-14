# RangeDeletionRenameRace

TLA+ specification of the rename / range-deleter / FilteringMetadataClearer race
described in **SERVER-114326**. SERVER-113667 shipped a symptom-level
quick-fix (skip `RangeDeleter` invalidation when the CSR UUID disagrees with the
`RangeDeletionTask` UUID); the spec models the underlying ordering bug that
makes the mismatch observable in the first place.

## What the spec models

Three threads share state on a single shard:

1. **RenameCoordinator** — commits the rename via two oplog entries:
   `COMMIT_RENAME` rewrites every pending `RangeDeletionTask.collectionUuid`
   from `OldUUID` to `NewUUID`; `CLEAR_METADATA` replaces the cached
   `CollectionShardingRuntime` metadata UUID.
2. **RangeDeleterServiceOpObserver** — fires `onUpdate` on
   `config.rangeDeletions` and reads `(task.uuid, metadata.uuid)` before
   deciding whether to invoke `invalidateRangePreservers()`.
3. **FilteringMetadataClearer** — applies the metadata clear/refresh that
   closes the window.

The shared state is `taskUUID` and `metadataUUID`. The bug toggle
`AllowCommitBeforeMetadataClear` controls whether `CLEAR_METADATA` may be
deferred past the observer's reads. With the toggle off, the spec models the
fix: the clear is sequenced before any observer action.

## Invariants

- `RangeDeleterMetadataMatchesCollectionUUID` (headline) — when the observer
  invokes `invalidateRangePreservers`, the task UUID it observed must equal the
  metadata UUID it observed.
- `ObserverSnapshotsCoherent` — after the observer has decided, its two
  snapshots must agree.
- `PostRenameMetadataIsFresh` — once the coordinator reaches `DONE`, the
  cached metadata UUID equals the task UUID.

## Configurations

| File                                  | Toggle                              | Expected outcome                                                        |
|---------------------------------------|-------------------------------------|-------------------------------------------------------------------------|
| `MCRangeDeletionRenameRace.cfg`       | `AllowCommitBeforeMetadataClear=FALSE` | All invariants hold; liveness holds.                                  |
| `MCRangeDeletionRenameRace_bug.cfg`   | `AllowCommitBeforeMetadataClear=TRUE`  | `RangeDeleterMetadataMatchesCollectionUUID` violated with counterexample. |

## Running

```sh
cd src/mongo/tla_plus
./model-check.sh Sharding/RangeDeletionRenameRace
```

## Paired jstest

`jstests/sharding/range_deletion_stale_metadata_after_rename.js` exercises the
300ms TOCTOU window described in the ticket against a real `ShardingTest`.
