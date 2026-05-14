# SERVER-124134 — Ingress Backpressure Under Thread Exhaustion

## Background

`SessionManagerCommon::startSession` (`src/mongo/transport/session_manager_common.cpp`)
is the single chokepoint where an accepted ingress connection is converted into a
`SessionWorkflow` and dispatched onto the service executor. Today the only refusal
gate at that site is a comparison against `_maxOpenSessions`, which is derived from
`min(RLIMIT_NOFILE * 0.4, serverGlobalParams.maxConns)`. In other words, the ingress
path treats *file descriptors* as the canonical scarce resource and ignores every
other pressure under which `startSession` actually runs.

In the synchronous transport mode the service executor lazily spawns one worker
thread per accepted session. In the asynchronous and gRPC modes the worker pool is
nominally bounded, but the accept loop has no visibility into the saturation level
of that pool — accept still succeeds, queued work piles up behind the bounded
workers, and outstanding accepts hold both kernel-side socket buffers and
server-side `SessionWorkflow` allocations.

## Problem

HELP-91997 captured the failure mode in production: a node continued to accept
connections and continued to call `pthread_create` after `pthread_create` was
already returning `EAGAIN` against `RLIMIT_NPROC` and the system-wide
`/proc/sys/kernel/threads-max`. From the operator's point of view the symptoms were:

1. `connections.current` kept climbing past any sensible bound while wall-clock
   command latency went unbounded.
2. The server consumed memory monotonically for the per-session arenas of accepted
   but undispatched sessions.
3. When thread creation finally raised, the unhandled error escalated into a
   fault — see the linked SERVER-124124, which is the downstream "what happens
   when we actually hit the limits" ticket.

Three pressures are unaccounted for at the limit check on
`session_manager_common.cpp:340`:

- **`RLIMIT_NPROC`** — per-user process/thread budget. The synchronous executor
  burns one entry per active session.
- **System-wide thread budget** (`/proc/sys/kernel/threads-max` on Linux) — shared
  with every other process on the host.
- **Recent thread creation failure rate** — even before either ceiling, repeated
  `EAGAIN` from `pthread_create` is the canary that the next `startSession` will
  also fail and the accept loop should stop drinking from the listen queue.

Lack of throttling and backoff against these pressures is, in effect, an
ingress-side DoS surface: an attacker (or a misbehaving client fleet) only needs
to open connections faster than they can be dispatched in order to push the node
into a state where graceful refusal is no longer reachable.

## Proposal

Add a new server parameter, `maxConnectionsPerWorkerHeadroom` (default `0.05`,
range `[0.0, 1.0]`), that expresses the minimum fraction of the worker pool that
must remain available before the accept loop will admit another non-priority
session. The check is layered into the existing condition at
`session_manager_common.cpp:342` so that the priority-port and CIDR-exempt
bypasses survive intact:

```
if ((sync.size() >= _maxOpenSessions ||
     workerUtilization() > 1.0 - gMaxConnectionsPerWorkerHeadroom ||
     recentThreadCreateFailureRate() > kThreadCreateFailureRateThreshold ||
     MONGO_unlikely(rejectNewNonPriorityConnections.shouldFail())) &&
    !isPrivilegedSession) {
    _sessions->incrementRejected();
    session->end();
    return;
}
```

When utilization crosses the headroom threshold the manager logs a single
structured `ConnectionRefused` event per refusal (rate-limited, reusing the
existing `LOGV2(22942)` channel) and increments a new
`sessionEstablishment.refusedForBackpressure` counter that surfaces in
`serverStatus.connections`. Priority and CIDR-exempt sessions never see the gate,
which preserves the operator's ability to reach a saturated node.

Above the headroom band the accept loop additionally applies a bounded
exponential backoff (`min(64ms, 1ms << k)`) capped at 8 retries before the
listener emits a refusal with a stable error code (`ErrorCodes::TooManyOpenConnections`),
giving load balancers a structured signal to redistribute rather than a TCP RST.

## Out of scope

This ticket adds the *gate*; the downstream crash-handling for the path where
`pthread_create` returns `EAGAIN` despite the gate is owned by SERVER-124124.
The two changes ship together but are sized separately.

## Test plan

- New regression-pin jstest at `jstests/noPassthrough/ingress_backpressure_audit.js`
  that documents the *current* behaviour under a connection storm so that any
  future regression in either direction (over-rejecting privileged sessions, or
  under-rejecting during saturation) shows up in CI.
- Existing `connection_establishment_rate_limiting_stats.js` continues to cover
  the queued-establishment path and is unchanged.
- Soak tests under `etc/perf/` will be extended in a follow-up to cover sustained
  storms against the new headroom knob.
