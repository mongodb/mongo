# SERVER-122751 design — heartbeat thread accounting must be bounded under asymmetric partition

## Root cause

`ReplicationCoordinatorImpl::_doMemberHeartbeat` schedules an outbound heartbeat
RemoteCommandRequest for every (target, interval) tuple without consulting how many
heartbeats to that same target are already in flight. The handle is added to
`_heartbeatHandles` and only removed when the callback fires — success, failure, or
cancellation. When the callback path also detects an unknown config version for the
target, `_scheduleHeartbeatReconfigResponse` schedules an *additional* follow-up
heartbeat to fetch that target's config.

Under an asymmetric partition (A can reach B; B cannot reply to A) the victim node
keeps *receiving* incoming heartbeats from the peers — so it keeps observing a
config-version mismatch on every interval — but its *outgoing* follow-up heartbeats
never complete: they sit at the executor until they time out, and the timeout itself
is bounded only by `electionTimeoutMillis`. Each interval therefore enqueues one new
handle while the previous interval's handle is still tracked. `_heartbeatHandles`
grows monotonically, `_maxSeenHeartbeatQSize` climbs without bound, and every
operation that has to walk `_heartbeatHandles` (which today is bounded only by the
size of that map) starts paying O(n) under `_mutex`. The same `_mutex` is taken on
the oplog-fetcher hot path; replication lag follows.

## Fix sketch

Introduce a per-target outstanding-heartbeat cap, enforced inside `_doMemberHeartbeat`
before `scheduleRemoteCommand`:

```
constexpr size_t kMaxOutstandingHeartbeatsPerTarget = 2;

// Under _mutex, counted via _heartbeatHandles values.
size_t outstanding = countHandlesForTarget(lk, target);
if (outstanding >= kMaxOutstandingHeartbeatsPerTarget) {
    // Drop the oldest in-flight handle to that target, or coalesce by skipping
    // this scheduling cycle entirely. Cancellation is preferred because it
    // returns the executor slot promptly and lets the next interval restart
    // from a clean state.
    cancelOldestHandleForTarget(lk, target);
}
```

The cap of 2 (one "in flight" + one "scheduled for the next tick") preserves the
existing semantics of overlapping a follow-up config-fetch heartbeat with the
periodic interval heartbeat, while making it structurally impossible for the queue
on any one target to grow past a small constant.

Two operational concerns:

1. **Drop vs coalesce.** Dropping the oldest cancels the executor handle and emits
   a `replSetHeartbeat.cancelled` log line; coalescing simply skips scheduling and
   leaves the existing handle to time out. Drop-oldest is preferred because the
   pending follow-up under partition is *known to be doomed* — its timeout would
   only delay recovery once the partition heals.
2. **`_maxSeenHeartbeatQSize` interpretation.** The metric already exists at
   `metrics.repl.heartBeat.maxSeenHandleQueueSize`. Post-fix it must stay bounded
   by `kMaxOutstandingHeartbeatsPerTarget * replicaSetSize`. This is what the
   new `heartbeat_thread_no_leak_under_asymmetric_partition.js` jstest asserts.

## Why not de-dupe by target alone

The question raised on the ticket is "why not one heartbeat thread per target with
a de-dupe check?". One-per-target is correct in steady state but fragile during
reconfig: a config-fetch heartbeat and an interval heartbeat to the same target
carry different timeouts and different response handlers; collapsing them into a
single in-flight slot would mean the reconfig path waits behind the interval
timeout. The proposed cap of 2 preserves the two semantic slots while still
bounding the queue.

## Spec / test surface

- jstest: `jstests/replsets/heartbeat_thread_no_leak_under_asymmetric_partition.js`
  — bridge-injected one-way partition, 60s sustain, asserts
  `metrics.repl.heartBeat.maxSeenHandleQueueSize` stays bounded and that the
  repl-coord thread inventory is O(replicaSetSize).
- Unit test surface (follow-up): extend
  `replication_coordinator_impl_heartbeat_v1_test.cpp` with a fixture that
  scripts repeated `_doMemberHeartbeat` calls without firing their callbacks,
  asserting that `_heartbeatHandles.size()` plateaus at the cap.
