# SnapshotOperationTimeOrdering

Formal specification of the snapshot / operationTime ordering hazard fixed in
[SERVER-120304](https://jira.mongodb.org/browse/SERVER-120304). The original
PR (#49451) was reverted (#53585) and re-landed as a redux (#53614); this
spec models the invariant that both attempts target.

## The hazard

A majority-read command runs in three phases on the server:

1. **AcquireSnapshot** — open a storage snapshot at the current committed
   timestamp.
2. **ExecuteRead** — read data from that snapshot.
3. **FetchOperationTime / SendResponse** — compute the `operationTime` returned
   to the client.

Pre-fix, step 3 called `replCoord->getCurrentCommittedSnapshotOpTime()`. If
a concurrent write advanced the committed snapshot between steps 1 and 3,
the reported `operationTime` exceeded the timestamp the read observed. A
causally-consistent follow-up read using that `operationTime` as
`afterClusterTime` returned a newer value, so a field appeared to change
between two consecutive reads with no intervening client write.

The fix captures the read's actual timestamp in the recovery unit at
transaction close and prefers it over the live committed snapshot.

## What the spec models

- `clusterTime` — monotone logical clock.
- `committedSnapshot` — currently-committed timestamp; can advance
  independently of any in-flight read (`AdvanceCommittedSnapshot`).
- Per-thread `threadPhase`, `threadSnapshot`, `threadOpTime` — the read
  pipeline state per concurrent client thread.
- `executedReads` — append-only history of `(snapshot, opTime)` pairs sealed
  on response.
- `UseLiveCommittedSnapshot` — bug toggle. `FALSE` models the fix
  (`getLastUsedReadTimestamp`); `TRUE` models the pre-fix code.

## Invariants

- `SnapshotMatchesOperationTime`: every completed read has
  `opTime <= snapshot`. This is the read-concern guarantee clients depend on.
- `ExecutedSnapshotBoundedByClock`: sanity bound on the cluster clock.
- `TypeOK`: structural well-formedness.

## Running

```
cd src/mongo/tla_plus
./download-tlc.sh
./model-check.sh Replication/SnapshotOperationTimeOrdering
```

With `UseLiveCommittedSnapshot = FALSE` (default cfg) all invariants hold.
Flip to `TRUE` to reproduce the bug counterexample: TLC reports a trace where
`AdvanceCommittedSnapshot` fires between `AcquireSnapshot` and
`FetchOperationTime` on the same thread, violating
`SnapshotMatchesOperationTime` on `SendResponse`.
