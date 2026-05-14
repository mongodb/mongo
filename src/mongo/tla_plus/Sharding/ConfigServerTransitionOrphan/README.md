# ConfigServerTransitionOrphan

Models the race between `transitionToDedicatedConfigServer` and an in-flight chunk migration
committing on the config-server-shard. Companion to [SERVER-125663][ticket].

[ticket]: https://jira.mongodb.org/browse/SERVER-125663

## Background

After SERVER-103990, the cluster member that hosts both the config server and a data shard drops
`config.rangeDeletions` as part of `transitionToDedicatedConfigServer`. A donor-side migration
commit performs four ordered steps:

1. Persist the commit decision and insert a `config.rangeDeletions` document with `pending: true`.
2. Register an in-memory `RangeDeletion` task in `TaskPending::kPending`.
3. Call `markAsReadyRangeDeletionTaskLocally`, which `$unset`s `pending` on the on-disk document.
   The OpObserver for that update calls `clearPending()` on the in-memory task and unblocks the
   deletion chain.
4. The deletion chain runs and orphan documents are removed.

If the transition drops `config.rangeDeletions` between steps 2 and 3, the `$unset` in step 3
raises `NoMatchingDocument`. The catch-block at `range_deletion_util.cpp:759` swallows it. The
OpObserver never fires, `clearPending()` is never called, the in-memory task is wedged in
`Pending` forever, and orphans on the former config-server-shard accumulate. A migration issued
with `waitForDelete = true` additionally blocks on the never-completing future.

## What this spec proves

* **Invariant `NoRangePermanentlyStuck`**: no in-memory range-deletion task ends in
  `StuckPending`.
* **Property `EveryPendingRangeEventuallyCleared`**: every committed migration's deletion chain
  eventually runs to completion.

A `CONSTANT AllowDropBeforeMarkReady` toggles the bug. With `FALSE` (green cfg) the transition
must wait for all in-flight migrations to reach `Ready` or `Cleared` before dropping the
collection; both properties hold. With `TRUE` (`_Bug.cfg`) the drop is unconditional and TLC
produces a 7-step counterexample exhibiting the SERVER-125663 hazard, anchoring the falsifier on
the real code path.

## Running the model-checker

```sh
cd src/mongo/tla_plus
./model-check.sh Sharding/ConfigServerTransitionOrphan                              # green
./model-check.sh Sharding/ConfigServerTransitionOrphan _Bug                         # bug
```

## Scope

The spec models the donor-side handoff and the transition drop only. Recipient-side logic,
routing, the critical section, and the wider migration protocol are covered by `MoveRange.tla`
and the `RangeDeletionsSecondaryNodes.tla` spec in this directory. The on-disk `pending` field
collapse is modelled as a boolean per range; the spec assumes the OpObserver's call to
`clearPending` is observable as the transition from `RangeRegistered` to `RangeReady`.
