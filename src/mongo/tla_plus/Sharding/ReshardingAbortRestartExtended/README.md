# ReshardingAbortRestartExtended

Sibling of `ReshardingAbortRestart` (wave 2). Same Jira parent —
[SERVER-125589](https://jira.mongodb.org/browse/SERVER-125589): "Disallow
kStartOrContinue restart from kAbortedWithoutPrepare to prevent silent data loss."

## What this spec adds over wave 2

Wave 2 modelled `TransactionParticipant` at one `(lsid, txnNumber)` over
`{kNone, kInProgress, kAbortedWithoutPrepare, kCommitted}` and elided
`kPrepared` / two-phase plus `txnRetryCounter`. This module reuses wave 2's
state, action and recovery vocabulary and extends along the two elided axes:

1. **kPrepared branch.** Adds `kPrepared`, `kCommittedAfterPrepare`,
   `kAbortedAfterPrepare`. The defect symmetric to SERVER-85170 — a sub-router
   driving `kStartOrContinue` against a `kPrepared` participant — is gated by
   `AllowResumeFromPrepared`. Green cfg refuses; bug cfg lets TLC produce a
   silent-loss trace where the prepared oplog entry stays durable while
   in-memory state transitions back to `kInProgress` and a later commit
   persists only the post-restart writes.

2. **txnRetryCounter + `kAbortedOnFirstStatement` (Mulrow proposal).** Adds
   the retry-counter dimension and a `kAbortedOnFirstStatement` terminal. A
   `firstStatementTouched` ghost discriminates the safe SERVER-46679
   StaleConfig retry (no sub-statement durably touched WiredTiger → restart by
   bumping the counter) from the unsafe SERVER-85170 resume (work was durably
   staged → restart forbidden, escalate). Invariant
   `FirstStatementRetryBumpsCounter` pins the proposal: any restart from
   `kAbortedOnFirstStatement` must increase `txnRetryCounter`.

## Invariants

- `NoCommitWithLostWrites` — generalised wave-2 headline. Holds in green,
  falsified in bug cfg.
- `RecoveryPathDoesNotCrash` — wave-2 carry-forward.
- `FirstStatementRetryBumpsCounter` — pins the Mulrow proposal.
- `PreparedAbortIsTerminal` — once `kAbortedAfterPrepare`, accepted writes
  stay discarded.
- `CounterMonotonicity` (property) — `txnRetryCounter` is non-decreasing.

## How to run

    cd src/mongo/tla_plus
    ./model-check.sh Sharding/ReshardingAbortRestartExtended

Swap `MCReshardingAbortRestartExtended_bug.cfg` over the green cfg to drive
the bug counterexample.

## Related

- `Sharding/ReshardingAbortRestart` (wave 2; kAbortedWithoutPrepare branch).
- AF-15971 (production crash that exposed the wave-2 defect).
- SERVER-85170 (introduced `kStartOrContinue`).
- SERVER-46679 (introduced the StaleConfig first-statement retry preserved
  here via `kAbortedOnFirstStatement`).
