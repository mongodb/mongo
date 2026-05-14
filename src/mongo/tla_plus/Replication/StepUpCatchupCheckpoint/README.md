# StepUpCatchupCheckpoint

Formal TLA+ specification covering the SERVER-126311 step-up catchup gate.

## Background

When a replica set member is elected primary, the server runs a post-election
"catch-up" phase before accepting writes. That phase ensures the new primary's
locally applied state has caught up with everything previously streamed to it.
The production code path under `disagg_storage/pali/log_server_manager.cpp`
contains a special case for "no checkpoint installed yet" that, prior to the
fix, returned success even when the oplog already contained streamed entries
that had not yet been applied to the local catalog. The new primary then
accepted writes against an effectively empty catalog (e.g. it created a fresh
`config.transactions` collection with a new UUID), and previously
majority-committed writes were dropped on the floor.

## What the spec models

* Three server nodes, each with `state` (`Leader` / `Follower`), `currentTerm`,
  an `oplog` sequence of `[term, lsn]` records, a `checkpointTS` (0 means no
  checkpoint installed yet), and a `lastApplied` lsn that can only advance
  past values that have already been checkpointed.
* A global `majorityCommitted` set of oplog entries that have been replicated
  to a quorum.
* Actions: election (`BecomePrimaryByMagic`), the catchup gate
  (`TryStepUpCatchup`), `ClientWrite`, `ReplicateOplog`, `InstallCheckpoint`,
  `StepDown`, and `CommitMajority`.
* A boolean CONSTANT `AllowBypassOnNoCheckpoint`. When `TRUE`, the gate
  matches the production bug (bypass succeeds even when the oplog is
  non-empty); when `FALSE`, the gate matches the proposed fix (refuse to
  step up until a checkpoint is installed).

## Invariants

* `NoLostWritesOnStepUp` - every majority-committed entry appears in the oplog
  of every current leader.
* `OplogLsnMonotone` - oplog lsn strictly increases by index.

## How to run

```
cd src/mongo/tla_plus

# Expected: PASS (proposed fix)
./model-check.sh Replication/StepUpCatchupCheckpoint \
    --config MCStepUpCatchupCheckpoint_green.cfg

# Expected: FAIL with a NoLostWritesOnStepUp counter-example trace
./model-check.sh Replication/StepUpCatchupCheckpoint \
    --config MCStepUpCatchupCheckpoint_bug.cfg
```

The bug configuration intentionally violates the safety invariant. The
counter-example trace TLC produces matches the failure sequence described in
the SERVER-126311 RCA: a candidate with non-empty oplog and `checkpointTS = 0`
takes the bypass, becomes leader, and writes a term-N+1 entry without first
applying the previously committed term-N entries that other nodes still hold.

## Notes on faithfulness

The spec abstracts a few details to keep model-checking tractable:

* Election is modelled as a one-shot atomic `BecomePrimaryByMagic` action that
  requires the candidate to be no further behind on streamed lsn than any
  voter, mirroring the freshness check at election time. The bug being modelled
  is not in the election rules; it is in the post-election catchup gate.
* The "new primary writes a fresh catalog with a new UUID" production symptom
  is abstracted into the simpler observation that a leader that stepped up
  without all majority-committed entries in its oplog will, on its next
  `ClientWrite`, hold an oplog that violates `NoLostWritesOnStepUp`. The TLA+
  invariant catches that window directly.
* `lastApplied` is constrained to advance only as a side effect of installing a
  checkpoint, modelling the production property that oplog application
  requires a known catalog.
