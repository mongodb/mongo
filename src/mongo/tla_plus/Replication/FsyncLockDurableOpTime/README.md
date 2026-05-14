# FsyncLockDurableOpTime

TLA+ specification of SERVER-126254: `fsyncLock` leaves `durableOpTime` stuck
behind `lastWritten`.

## What this spec models

A single-node primary with three pieces of state:

- `lastWritten` — most recent optime committed by a `WriteUnitOfWork` in memory.
- `durableOpTime` — most recent optime fsynced to the journal.
- `globalSLockHeld` — whether `fsyncLock` is holding Global S.

Three actions drive forward progress: `CommitWUOW` advances `lastWritten`,
`AcquireGlobalS` flips the lock, `AdvanceDurable` catches `durableOpTime` up
to `lastWritten`. `ReleaseGlobalS` is the dual of `AcquireGlobalS` so the
state graph is closed under recovery.

The bug is toggled by the `BugEnabled` constant. When `TRUE`, `AdvanceDurable`
is gated by `~globalSLockHeld` — the JournalFlusher cannot advance
`durableOpTime` while Global S is held, matching the documented stable
equilibrium. When `FALSE`, the gate is removed; this is the fix.

## Properties checked

Safety:

- `DurableNeverAheadOfWritten` — `durableOpTime <= lastWritten` always.
- `TypeOK`, `MonotoneOpTimes` — sanity.

Liveness:

- `DurableEventuallyCatchesUp` — `lastWritten > durableOpTime ~> equal`.
- `NoWedgeUnderGlobalS` — diagnostic variant scoped to the lock-held window.

Both liveness properties hold under the green cfg (`MCFsyncLockDurableOpTime.cfg`,
`BugEnabled = FALSE`) and are violated under the bug cfg
(`MCFsyncLockDurableOpTimeBug.cfg`, `BugEnabled = TRUE`).

## Running

```
cd src/mongo/tla_plus
./download-tlc.sh                                    # one-time
./model-check.sh Replication/FsyncLockDurableOpTime  # green
```

To check the bug cfg, point TLC at the bug harness directly:

```
cd src/mongo/tla_plus/Replication/FsyncLockDurableOpTime
java -cp ../../tla2tools.jar tlc2.TLC -workers auto \
    -config MCFsyncLockDurableOpTimeBug.cfg MCFsyncLockDurableOpTimeBug.tla
```

TLC should report "Temporal properties were violated" with a counterexample of
length 3 (CommitWUOW; AcquireGlobalS; stutter), which is the wedge between
`fsyncLock` acquisition and `fsyncUnlock` release exactly as captured in the
deterministic repro on the ticket.

## Companion regression test

`jstests/replsets/fsync_lock_durable_optime_stuck.js` promotes the
attached SERVER-126254 repro to a regression test. The jstest pauses the
`JournalFlusher` thread via failpoint, issues a `w:1, j:false` insert,
calls `fsyncLock`, asserts the wedge, and verifies the fix's release path.
