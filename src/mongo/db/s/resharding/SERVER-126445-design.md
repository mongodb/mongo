# SERVER-126445 — Remove waiting for change streams monitor from donor critical section

## Problem

When resharding verification is enabled, the donor creates a `ReshardingChangeStreamsMonitor`
that tails the source collection's change stream and counts inserts/updates/deletes to compare
against the recipient post-commit. Until this ticket, the donor `awaited` that monitor's
completion future *inside* `_runUntilBlockingWritesOrErrored`, immediately after
`_writeTransactionOplogEntryThenTransitionToBlockingWrites` had acquired the recoverable
critical section and transitioned the donor to `kBlockingWrites`. The relevant chain in
`resharding_donor_service.cpp` was:

```
_runUntilBlockingWritesOrErrored
  -> _writeTransactionOplogEntryThenTransitionToBlockingWrites      // critsec acquired
  -> _awaitChangeStreamsMonitorCompleted                            // <-- blocking inside critsec
```

The monitor drains the final tail batch from the change stream after writes are blocked, and
under load this tail can be tens to hundreds of milliseconds (the `kCriticalSection` →
`kChangeStreamMonitor` cross-phase elapsed metric, `blockingWritesToMonitorCompletionSecs`,
exists specifically to surface this delay). For the duration of that wait the user-visible
critical section stays held — every read/write to the source namespace gets
`InterruptedDueToReshardingCriticalSection`.

## Fix

Move `_awaitChangeStreamsMonitorCompleted` out of the chain that runs while writes are
blocked. It now lives in `_finishReshardingOperation`, after
`ShardingRecoveryService::releaseRecoverableCriticalSection` has been called on the source
namespace. The donor still drives the monitor to completion before it removes its state
document and reports `kDone` to the coordinator (the monitor's output, the documents delta,
is still required for verification), but writes are no longer blocked during the drain.

New chain:

```
_runUntilBlockingWritesOrErrored
  -> _writeTransactionOplogEntryThenTransitionToBlockingWrites      // critsec acquired
                                                                    // (no more monitor wait here)
_notifyCoordinatorAndAwaitDecision                                  // unchanged
_finishReshardingOperation
  -> ... releaseRecoverableCriticalSection ...                      // critsec released
  -> _awaitChangeStreamsMonitorCompleted                            // <-- monitor wait now here
  -> _updateCoordinator + _removeDonorDocument
```

Sibling ticket SERVER-126444 moves the coordinator's consumption of the monitor's output
(the documents-delta verification) to a post-commit, post-critical-section path on the
coordinator side, so the donor's local wait no longer needs to land before the coordinator
commits.

## Invariants preserved

- `_changeStreamsMonitor` is still fully drained on the donor before the donor doc is
  removed. The existing `_changeStreamsMonitorQuiesced` future in `_runMandatoryCleanup`
  remains the final guarantor on shutdown / error paths.
- The `_changeStreamsMonitorCompleted` promise is still fulfilled with the documents delta
  on success and with the error status on failure; only the call site of the fulfilling
  `.then()` chain changed.
- The `kCriticalSection` metric is still ended at the same place
  (`_finishReshardingOperation`), and the cross-phase elapsed metric
  `blockingWritesToMonitorCompletionSecs` continues to be reported; under the new behavior
  its value will trend toward zero on the donor side, which is the intended effect.
- The abort / step-down paths in `onError` and `_runMandatoryCleanup` already fulfilled the
  monitor promises with the propagated status; nothing about those paths changed.

## Test

`jstests/sharding/resharding_donor_critical_section_does_not_wait_for_change_streams_monitor.js`
pins the new ordering. It:

1. Enables verification on a `moveCollection`-shaped resharding op so the monitor is created.
2. Engages `hangReshardingChangeStreamsMonitorBeforeReceivingNextBatch` on the donor.
3. Waits for the donor to reach `donorState=blocking-writes` and for the failpoint to trip.
4. Asserts that the donor transitions PAST `blocking-writes` to `done` (or removes its doc)
   while the monitor is still hung — i.e., the critical section was released before the
   monitor wait. Pre-fix this assertion times out because the donor was parked inside the
   critical section waiting for the monitor.
5. Lifts the failpoint so the operation can finish.
