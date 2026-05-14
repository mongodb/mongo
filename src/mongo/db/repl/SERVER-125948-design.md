# SERVER-125948 -- Ensure interruptibility for `stepUpIfEligible`

## Root cause

`ReplicationCoordinatorImpl::stepUpIfEligible` (defined in
`replication_coordinator_impl_step_up_step_down.cpp`) already accepts an
`OperationContext*`, but the long-running waits it transitively dispatches do
not thread it through. The pathological waits are:

- `connectAndCatchUp` / `waitUntilOpTimeForReadUntil` -- bounded only by the
  static `gOplogCatchUpForStepUpTimeoutMillis` knob.
- `OplogApplicationCoordinator::drainAndStop().wait()` -- unbounded.
- `cancelStateMachine` + `stateMachineJoin` -- unbounded.
- `waitForCheckpointsToBeInstalledForStepUp` -- unbounded; this is the
  signature wedge observed in BF-43105 run 37, where
  `CheckpointManager::_installCheckpoint` was blocked on a `getPageAtLSN`
  with no per-call interrupt path.
- SLS `accept(kStepUp)` -> `finalizeStepUp` (gRPC append-log uses a 10s
  deadline; everything else is I/O- and CPU-bound with no cap).
- Step-up primary hooks (`onStepUpBegin`, `onStepUpComplete`, index-build
  coordinator startup, system-index verification).

Each of these blocks on a bare `stdx::condition_variable::wait` (or an
equivalent `Future::wait()` / `EventHandle::waitForEvent`) keyed on a member
mutex. When `shutdown(force=true)` fires concurrently, the global
`OperationContext::markKilled` propagates to every running opCtx, but these
bare waits never check it. Net effect: `_steppingUp` stays latched, the
shutdown thread blocks on `joinReplicationCoordinator`, and the process
wedges until the test harness's idle timeout (~7 min) kills it. BF-43105
reports 5/50 = 10% reproduction rate under `disagg_pali_chaos`.

## Fix

Two coordinated changes, neither invasive:

1. **Thread `opCtx` through every wait.** Replace bare condvar waits with
   `opCtx->waitForConditionOrInterrupt(cv, lk, predicate)` and replace
   `Future::wait()` calls with `Future::waitNoThrow(opCtx)` /
   `Future::get(opCtx)`. For `_replExecutor->waitForEvent(finishEvent)` use
   the `(opCtx, event)` overload that already exists on `TaskExecutor`. The
   sweep covers `connectAndCatchUp`, the drain wait,
   `waitForCheckpointsToBeInstalledForStepUp`, `stateMachineJoin`, and the
   SLS `accept` future.

2. **Make the function's opCtx the cancellation root.** Today
   `stepUpIfEligible` receives an opCtx but mostly stashes it. Reuse it as
   the explicit cancellation token for the SLS state machine handoff
   (`CancellationToken::uncancelable()` -> `CancellationToken::fromOpCtx`).
   On shutdown the global interrupt source flips that token, the SLS
   coroutines unwind cleanly, and `_steppingUp` is cleared via the existing
   `ON_BLOCK_EXIT` at line 898.

`OperationContext::waitForConditionOrInterrupt` is the canonical pattern
across the codebase (see SERVER-126425 `WCRetry` interruptibility for the
shape). No new failpoints required; the `hangInStepUpIfEligible` failpoint
in the accompanying jstest is added solely to deterministically freeze a
step-up so the test can assert shutdown still progresses. The jstest
asserts mongod exits within 30s once shutdown is issued, which is well
inside the ReplSetTest default 5-minute hang detector and well under the
~7 min wedge observed in production.
