# SERVER-126462: Early-reject backoff for write-conflict storms

Status: design proposal (prototype)
Owner: Query Execution
Epic: SPM-4627
Related: SERVER-65418 (release-ticket-before-backoff)

## Problem

When N concurrent ops contend on a hot document, the release-ticket path
(`internalQueryEnableWriteConflictBackoffWithoutTicket=true`) creates a
positive-feedback storm: a sleeping op drops its write ticket, the
dynamic concurrency controller observes free capacity, admits the next
queued writer, that writer also conflicts on the same document, and the
pool refills with non-productive ops. CPU is burned on admission,
scheduling, and WT transaction setup for ops guaranteed to retry.

The Jira investigation comment (2026-05-12) rules out two prior
candidates:

1. **Pool-size circuit breaker.** Threshold scales with pool size; under
   storm load the controller expands the pool faster than the breaker
   can fire. Measured: pool 95-128, breaker requires 47-64 concurrent
   waiters, workload produces at most 21. Never fires.
2. **Queue-depth activation gate.** Release-ticket drops queue depth to
   zero during sleeps, so the gate stays false through a severe storm.

What survives empirically: a two-layer defense at different points in
the retry lifecycle — `releaseBackoff` (mode switch) then `maxRetry`
(hard shed).

## Proposal

Replace the singleton `internalQueryEnableWriteConflictBackoffWithoutTicket`
boolean with a two-threshold escalation on per-op consecutive WCE count
(`writeConflictsInARow`, already tracked in `PlanExecutorImpl`):

| Phase | `writeConflictsInARow` range | Behavior            | Rationale                                          |
|-------|------------------------------|---------------------|----------------------------------------------------|
| A     | `[0, releaseBackoff)`        | release-ticket      | Isolated transient conflict; donate slot.          |
| B     | `[releaseBackoff, maxRetry)` | hold-ticket         | Storm regime; occupy slot to throttle admission.   |
| C     | `>= maxRetry`                | shed (TempUnavail)  | Stuck op; free slot, push retry to client.         |

Both thresholds become query knobs. Defaults proposed:

- `internalQueryWriteConflictReleaseBackoff` = 4
- `internalQueryWriteConflictMaxRetry` = 50

`INT_MAX` for `releaseBackoff` reproduces the current release-everywhere
default; `0` reproduces the legacy hold-everywhere path. Existing
boolean is kept as a deprecated alias that forces `releaseBackoff` to
0 or `INT_MAX` for one release cycle.

### Integration point

`PlanExecutorImpl::_handleNeedYield` (`plan_executor_impl.cpp:500`),
the existing branch that reads
`internalQueryEnableWriteConflictBackoffWithoutTicket.load()`. Replace
the boolean check with:

```cpp
const auto releaseBackoff = internalQueryWriteConflictReleaseBackoff.load();
const auto maxRetry       = internalQueryWriteConflictMaxRetry.load();

if (writeConflictsInARow >= static_cast<size_t>(maxRetry)) {
    // Phase C: shed.
    throwTemporarilyUnavailableException(
        "Write-conflict retry cap reached (maxRetry={})", maxRetry);
}

if (writeConflictsInARow < static_cast<size_t>(releaseBackoff)) {
    // Phase A: defer log+backoff to the yield handler (release-ticket).
    _writeConflictsInARowToLog = writeConflictsInARow;
} else {
    // Phase B: log+backoff now, while holding the ticket.
    logWriteConflictAndBackoff(writeConflictsInARow);
}
```

User-facing only — `_yieldPolicy->canAutoYield()` already gates this
code path; internal ops and secondaries that disable auto-yield bypass
the cap.

### Error code

Reuse `ErrorCodes::TemporarilyUnavailable`. Drivers already treat it as
retryable with backoff, which is exactly the client-side behavior we
want for an op the server has declined to keep retrying. A dedicated
`WriteConflictStorm` code was considered and rejected: it requires
driver work and breaks proxy clients that whitelist retryable codes.

### What this does NOT change

- Internal ops, secondaries, applyOps, replication oplog tailing — none
  of these flow through `PlanExecutorImpl::_handleNeedYield`.
- Multi-statement transactions: the cap fires per statement; the
  transaction itself follows the existing `TransientTransactionError`
  contract.
- Per-collection scoping (Option 3 in the Jira). Deferred to v2.

## Why this resolves the storm

Phase B is the load-bearing change. Under release-ticket, a sleeping
op vacates its slot for the next queued writer, which is statistically
also storm-bound on the same document. Under hold-ticket, the slot
stays occupied, the concurrency controller observes high utilization,
admission slows, and each retry sees fewer competitors. The
self-throttling is automatic — no global state, no breaker that needs
to detect the storm explicitly.

Phase C is the backstop. A permanently-hot document (e.g. queue-head
pattern) would otherwise pin Phase-B ops indefinitely, holding their
tickets forever. Shedding at `maxRetry` returns the slot to the pool
and pushes the retry decision to the client, which has a wider
backoff budget than the server.

## Test plan

A jstest at
`jstests/noPassthrough/query/write_conflict_storm_early_reject.js`
pins the storm-prevention behavior end-to-end:

1. Launch a single-node replset with knobs set to small thresholds
   (`releaseBackoff=2`, `maxRetry=5`).
2. Open a multi-statement transaction holding a write on `{_id: 0}`.
3. From `K = maxRetry * 3` parallel shell connections, issue
   `findAndModify` updates on `{_id: 0}` with `maxTimeMS` large enough
   that the server-side cap fires before the deadline.
4. Assert:
   - At least one connection observes
     `ErrorCodes.TemporarilyUnavailable` (the cap fired).
   - The serverStatus `metrics.operation.writeConflicts` counter
     grew but did not exceed `K * maxRetry` (each op shed at the cap,
     not at the legacy unlimited retry).
   - After the holder commits, all surviving connections succeed
     within a bounded retry window.

Unit-test seam (follow-up): `PlanExecutorImpl::_handleNeedYield` is
not currently exposed for direct unit testing — the prototype CL will
add a friend-test shim so the three-phase branch decision can be
exercised without a live storage engine.

## Rollout

1. Land knobs + branch logic with `releaseBackoff = INT_MAX`,
   `maxRetry = INT_MAX`. No behavior change; surface area opens.
2. Bench `high_cpu_degradation` workload at
   `releaseBackoff ∈ {2, 4, 8}` × `maxRetry ∈ {20, 50, 100}`. Pick the
   combination that recovers the ~40-50% throughput regression
   without inflating p99 on isolated-conflict microbenchmarks.
3. Flip defaults in a separate PR with the perf numbers attached.

## Open questions

- Should `maxRetry` shedding emit a structured log event distinct from
  the existing `logWriteConflictAndBackoff` cadence, so SREs can
  alert on shed rate per replica set?
- Interaction with prepared transactions: `_handleNeedYield` runs
  before the yield, but `kPrepareConflict` flows through a different
  branch. Confirm no double-counting against `writeConflictsInARow`.
