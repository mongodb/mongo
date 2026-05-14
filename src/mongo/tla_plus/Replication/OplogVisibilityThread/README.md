# OplogVisibilityThread

TLA+ specification of the oplog visibility thread lifecycle, driven by the
side effect of `WiredTigerRecordStore::Oplog::Oplog(...)` / `~Oplog()`, which
call `WiredTigerOplogManager::start()` / `::stop()`.

## What this models

A single `WiredTigerOplogManager` owns one visibility thread. The thread is
started when a fresh `Oplog` record store is constructed (today: as a side
effect of `getRecordStore()` during durable recovery) and stopped when that
record store is destroyed. When DDL on the oplog forces a recovery, multiple
concurrent queries that call `getRecordStore()` can race through the
construction path. Without serialization at the catalog level, two readers
can interleave `start()` and `stop()` on the same `OplogManager`, and the
visibility thread can be torn down while a reader still holds a pin on the
record store.

`OplogVisibilityThread.tla` models a small set of `Readers` driving the
lifecycle. The boolean constant `AllowConcurrentStartStop` toggles the bug:

- `FALSE` (green) -- the start/stop critical section is serialized; all
  invariants hold.
- `TRUE` (bug) -- readers race; TLC produces counterexamples for both
  `AtMostOneStartStopInFlight` and `NoThreadTeardownWhileReaderPinned`.

## Invariants

- `AtMostOneStartStopInFlight` -- only one reader at a time mutates the
  manager's lifecycle.
- `NoThreadTeardownWhileReaderPinned` -- a pinned reader never observes
  `threadState` in `stopped` or `stopping`.
- `PinnedReaderSeesRunningThread` -- a reader that pinned at the current
  epoch never sees `threadState = "stopped"`.
- `EpochMonotonic` -- `oplogEpoch` is non-decreasing.
- `TypeOK` -- variable types.

## Running

```
cd src/mongo/tla_plus
./download-tlc.sh                            # one-time
./model-check.sh Replication/OplogVisibilityThread
```

For the bug configuration:

```
cd src/mongo/tla_plus/Replication/OplogVisibilityThread
java -cp ../../tla2tools.jar tlc2.TLC \
    -config MCOplogVisibilityThread_bug.cfg \
    MCOplogVisibilityThread.tla
```

Green run prints `Model checking completed. No error has been found.`; bug
run halts with an invariant-violation trace.

## Companion jstest

`jstests/replsets/oplog_visibility_concurrent_start_stop.js` exercises the
race via `pauseJournalFlusherThread`, repeated `replSetResizeOplog`, and
concurrent reverse-cursor oplog readers; asserts reads stay available.

Tracked under SERVER-122142.
