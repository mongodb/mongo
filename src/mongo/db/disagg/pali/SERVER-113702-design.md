# SERVER-113702: PSR clean shutdown

## Problem

The Page Server Reader (PSR) under the PALI subsystem has no clean shutdown path. When
the parent service tears the PSR down today, both halves of in-flight state are lost:

1. Requests sitting in the pending queue are dropped silently. The originating client
   sees a connection reset rather than a structured error, retries hammer the dying
   service, and resources allocated for the read (page-cache pins, buffer reservations)
   leak until the worker thread is forcibly torn down.
2. In-flight requests on worker threads are not given a chance to either complete or
   abort with a structured `ShutdownInProgress` status. Worker threads are not joined
   in a defined order, so log lines from the dying PSR interleave unpredictably with
   the rest of the shutdown sequence and post-mortem analysis is painful.

Both failure modes are modeled in `src/mongo/tla_plus/Disagg/PSRShutdownLiveness/` and
TLC produces a counter-example trace when `EnableDrain = FALSE`
(`MCPSRShutdownLiveness_bug.cfg`).

## Design

Shutdown is a monotonic five-phase state machine. Phases match the TLA+ spec
one-to-one.

| Phase            | Behaviour                                                                 |
|------------------|---------------------------------------------------------------------------|
| `Running`        | Normal operation. Clients enqueue, workers pick up, workers complete.     |
| `StopAccepting`  | New `submit()` calls return `Status(ErrorCodes::ShutdownInProgress, ...)` before the request touches the queue. Existing pending requests remain. |
| `Draining`       | The queue is closed. Workers continue picking pending requests off the queue and serving them until the drain deadline (`psrShutdownDrainDeadlineMs`, default 5000ms) elapses or the queue empties. |
| `Cancelling`     | Any request still in `pending` is resolved with `ShutdownInProgress`. Each worker servicing an in-flight read is cooperatively interrupted; the worker observes the cancel and returns `ShutdownInProgress` to its client. |
| `Joining`        | Once `pending` is empty and no in-flight requests remain, the worker pool is joined in deterministic order. After all workers join the PSR transitions to `Down`. |

The drain deadline is observable: a request that races the cancel signal and wins is
stamped `Completed`; a request that loses (or that was never picked up) is stamped
`Canceled`. Either status is a clean terminal state. `Orphaned` is the bug-marker
status in the spec; production code never produces it.

### Observability hooks

Every phase transition emits a single `LOGV2` line on component `kStorage` (or a new
`kDisagg` component if one is added) at `kInfo` severity:

```
"PSR shutdown phase transition" phase="StopAccepting" pendingCount=N inFlightCount=M
"PSR shutdown phase transition" phase="Draining"      pendingCount=N inFlightCount=M elapsedMs=T
"PSR shutdown phase transition" phase="Cancelling"    pendingCount=N inFlightCount=M cancelDeadlineMs=D
"PSR shutdown phase transition" phase="Joining"       workersToJoin=K
"PSR shutdown phase transition" phase="Down"          totalCompleted=C totalCanceled=X durationMs=T
```

Counter-style FTDC metrics mirror the same view: `psr.shutdown.completedDuringDrain`,
`psr.shutdown.canceledPending`, `psr.shutdown.canceledInFlight`, `psr.shutdown.durationMs`.

### Client-side categorization

The cancel paths return one structured error so clients can route retries correctly.
The error is `ShutdownInProgress` with an `errInfo` extension carrying:

```
{ stage: "pending" | "in_flight", psrId: "<oid>", shutdownStartedAt: <Date> }
```

Clients that observe `stage = "pending"` can safely retry against a fresh PSR
immediately; `stage = "in_flight"` retries should respect a small back-off because the
read was already consuming pages.

### What the TLA+ spec proves

- Safety `NoOrphanedRequest` -- under the proposed phase machine, no observed request
  is ever discarded without a terminal status.
- Safety `ShutdownCompletesCleanly` -- once `phase = Down`, every observed request is
  either `Completed` or `Canceled`; nothing is left in `pending` or in-flight.
- Liveness `EveryRequestResolved`, `ShutdownTerminates`,
  `AllWorkersEventuallyJoined` -- once shutdown is signaled, the protocol always
  finishes; workers are joined and the PSR reaches `Down`.

The bug `cfg` (`EnableDrain = FALSE`) enables `BugSkipDrainAndCancel`, modelling the
current "stop accepting then immediately join" behaviour. TLC reports a trace where
at least one request ends in `done[r] = Orphaned`, violating `NoOrphanedRequest`.

## Out of scope

- Retry-budgeted reconnection on the client side (tracked separately).
- Tearing down the PSR's underlying storage handle. Handle ownership is upstream of
  this protocol and is unchanged.
- Coordinating shutdown across multiple PSRs in the same process. Each PSR runs this
  state machine independently.
