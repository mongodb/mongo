# ReshardingAbortRestart

TLA+ specification for [SERVER-125589](https://jira.mongodb.org/browse/SERVER-125589):
"Disallow kStartOrContinue restart from kAbortedWithoutPrepare to prevent silent data loss."

## What this spec models

A coordinator-side subset of the `TransactionParticipant` state machine at a single
`(lsid, txnNumber)`, plus the two-block recovery path in
`txn_two_phase_commit_cmds.cpp::coordinateCommitTransaction` (the local-participant decision
recovery branch). The states modelled are `kNone`, `kInProgress`, `kAbortedWithoutPrepare`,
`kCommitted`; the actions are `beginOrContinue(kStart|kContinue|kStartOrContinue)`,
`abortTransaction`, and `commitTransaction`, plus the two recovery blocks and the gap between
them. Prepared / two-phase branches and `txnRetryCounter` are intentionally elided - they do not
participate in the bug.

The defect surfaced first on the coordinator recovery path (AF-15971: the
`invariant(!txnParticipant.transactionIsOpen())` crash), but the root cause is a participant
state-machine transition introduced by SERVER-85170: `kAbortedWithoutPrepare -> kInProgress` at
the same `txnNumber` is licensed when the action is `kStartOrContinue`. That transition is what
SERVER-125589 removes.

## Bug toggle and the headline invariant

`AllowResumeFromAbortedWithoutPrepare` (constant; `FALSE` in `MCReshardingAbortRestart.cfg`,
`TRUE` in `MCReshardingAbortRestart_bug.cfg`) gates the unsafe transition. The headline invariant
is `NoCommitFromAbortedWithoutPrepare`: every commit must persist exactly the writes the user
submitted under `(lsid, txnNumber)`. In the green model TLC finds no violation. In the bug model
TLC produces the silent-data-loss trace from the Jira description (begin -> write `w1` -> abort
-> sub-router `kStartOrContinue` + `w2` -> commit; `stagedForCommit = {w2}` while
`submitted = {w1, w2}`).

Two supporting invariants ride along: `RecoveryPathDoesNotCrash` (the AF-15971 crash signature)
and `TerminalMonotonicity` (the pre-2024 protocol invariant called out by J. Mulrow).

## How to run

    cd src/mongo/tla_plus
    ./model-check.sh Sharding/ReshardingAbortRestart

To exercise the bug model, copy `MCReshardingAbortRestart_bug.cfg` over
`MCReshardingAbortRestart.cfg` before running.

## Related

- AF-15971 (production crash that exposed the defect)
- SERVER-85170 (introduced `kStartOrContinue`)
- SERVER-40808 (added the recovery-path invariant; sound in 2019)
- `jstests/sharding/resharding_no_resume_from_aborted_without_prepare.js` (companion repro)
