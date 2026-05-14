# ReshardingFCVDowngradeOrphans

Combined TLA+ specification for three related orphan/temp-collection bugs that arise
when a `setFeatureCompatibilityVersion` (setFCV) downgrade interleaves with an
in-flight `reshardCollection` or `moveChunk` migration. Each ticket exposes the
same skeleton — a multi-phase write across the global catalog plus a per-shard
`config.rangeDeletions` table, interrupted between writes by setFCV's abort path —
but differs in which derived state the abort hook fails to clean up. The spec models
the three failure surfaces simultaneously so the interactions can be checked as one
state machine rather than three independent ones.

## Discharged invariants

| Invariant                                          | Ticket           | Bug toggle          | What it pins                                                                                                  |
|----------------------------------------------------|------------------|---------------------|---------------------------------------------------------------------------------------------------------------|
| `TempCollectionMetadataClearedOnFCVAbort`          | SERVER-111230    | `BugTempCollLeak`   | Aborting a reshard whose donor is in `donatingInitialData` must wipe `tempCollMeta` before FCV settles.       |
| `RangeDeletionsClearedOnReshardAbort`              | SERVER-92437     | `BugRangeDelLeak`   | The setFCV abort path must synchronously clear `config.rangeDeletions` for any migration aborted in-flight.   |
| `NoStaleConfigChunksAfterFCVDowngrade`             | SERVER-121914    | `BugStaleChunks`    | `config.chunks` entries pinned to the temp namespace must clear when the owning reshard is aborted.           |

A fourth composite invariant — `IndexBuildDecisionFCVConsistent` — pins the
SERVER-92437 root anomaly directly: a recipient that skipped `buildingIndex` under
the feature flag must not commit under FCV `lastLts`. This is the precondition that
makes the range-deletion leak possible.

## Model files

- `ReshardingFCVDowngradeOrphans.tla` — the core spec (~350 lines).
- `MCReshardingFCVDowngradeOrphans.tla` — model-check harness (symmetry, bait predicates).
- `MCReshardingFCVDowngradeOrphans.cfg` — green config, all bug toggles off.
- `MCReshardingFCVDowngradeOrphans_Bug111230.cfg` — flips `BugTempCollLeak`, expects
  `TempCollectionMetadataClearedOnFCVAbort` to fail.
- `MCReshardingFCVDowngradeOrphans_Bug92437.cfg` — flips `BugRangeDelLeak`, expects
  `RangeDeletionsClearedOnReshardAbort` to fail.
- `MCReshardingFCVDowngradeOrphans_Bug121914.cfg` — flips `BugStaleChunks`, expects
  `NoStaleConfigChunksAfterFCVDowngrade` to fail.

## Running

```
cd src/mongo/tla_plus
./model-check.sh Sharding/ReshardingFCVDowngradeOrphans
```

The harness defaults to the green config. To re-falsify a single bug, copy the
matching bug `.cfg` over `MCReshardingFCVDowngradeOrphans.cfg` and re-run.

## State-space notes

`MaxOps` caps reshard + migration starts; with the default
`Shards = {s1, s2}, Namespaces = {n1}, MaxOps = 6`, model-checking completes in
seconds. Symmetry on `Shards × Namespaces` removes equivalent interleavings.
The migration coordinator and reshard donor/recipient run concurrently with the
FCV state machine — interleaved by TLA+'s native step semantics — which is what
makes the `FCV writes downgrade marker BEFORE issuing abort` window observable.

## jstest companion

`jstests/sharding/resharding_fcv_downgrade_orphan_cleanup.js` exercises the same
three failure modes against a running sharded cluster — temp-collection metadata
leak, pending range-deletion leak, and stale `config.chunks` entries — using
failpoints to force the abort-window interleavings the spec exhibits.
