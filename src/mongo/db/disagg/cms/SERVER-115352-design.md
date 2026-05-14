# SERVER-115352: CMS PSE lookup must have bounded timeout

## Problem

`InSLSConfigManager::subscribeToPageServers()` opens a streaming
`GetPageServingExtentList` RPC against each PSE peer and pulls entries via
`reader->Read()`, terminating on `reader->Finish()`. The `ClientContext`
carries no deadline. A peer that accepts the stream and then stops producing
entries (slow / hung / partitioned-but-not-disconnected) parks the calling
thread on `Read()` forever. On startup, writes are gated on the subscribe
loop completing, so a single slow peer stalls every write on the node.

## Design

### 1. Per-attempt deadline

Add a server parameter:

```
cmsPseLookupTimeoutMs (int, default 5000, range [100, 60000])
```

Before each `GetPageServingExtentList` attempt, set
`ClientContext::set_deadline(now + cmsPseLookupTimeoutMs)`. The deadline
applies to the entire attempt — stream open, every `Read()`, and the
terminating `Finish()` — so a peer that hangs at any phase trips
`DeadlineExceeded` in bounded time. Re-opening the stream on retry resets
the deadline.

### 2. Structured error classification

When the deadline trips (or `Finish()` returns non-OK), classify the
failure before logging / metricizing:

| Class              | When                                                                 |
|--------------------|----------------------------------------------------------------------|
| `TimeoutExceeded`  | Interrupt or shutdown raced the lookup; treat as transient.          |
| `PeerUnreachable`  | Stream open never completed (peer refused / TCP-level failure).      |
| `PeerSlow`         | Stream opened, deadline tripped while parked in `Read()`/`Finish()`. |

Classification is derived from the phase the deadline tripped in
(`opening` → `PeerUnreachable`, `reading`/`finishing` → `PeerSlow`) plus
the gRPC status on `Finish()`. Downstream callers branch on class: a
`PeerUnreachable` peer is dropped from the active set, a `PeerSlow` peer
is retried with backoff, and `TimeoutExceeded` propagates interruption
upward.

### 3. Observability

Add a counter:

```
metrics.disagg.cms.pseLookupTimeouts (Counter64)
```

incremented once per `DeadlineExceeded` (per attempt, not per lookup).
Log lines on timeout carry `{peer, attempt, elapsedMs, class}` at
`kWarning`. The existing per-class breakdown rides on the same counter
via a labelled child so operators can tell `PeerUnreachable` from
`PeerSlow` without parsing logs.

### 4. Retry policy

The subscribe loop already retries; the fix only bounds the per-attempt
blast radius. Retries are capped by the existing `cmsSubscribeMaxAttempts`.
Once retries exhaust on a `PeerSlow` peer, the subscribe call returns an
error rather than parking the thread.

## Correctness

The TLA+ spec at
`src/mongo/tla_plus/Disagg/CMSPSELookupTimeout/CMSPSELookupTimeout.tla`
models the lookup state machine with a parametric `TimeoutEnabled` flag.
Under the fix configuration TLC verifies `LookupEventuallyTerminates`:
every lookup eventually reaches a terminal phase in `{Succeeded,
TimedOut, Unreachable, GaveUp}`. Under the bug configuration
(`TimeoutEnabled = FALSE`, no deadline on the `ClientContext`) TLC
produces a counter-example: a lookup against a slow peer stays parked in
`PhaseReading` forever — the exact shape of the stall in the ticket.
