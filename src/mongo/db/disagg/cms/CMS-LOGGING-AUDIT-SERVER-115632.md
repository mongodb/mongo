# CMS Logging Audit (SERVER-115632)

Observability audit of the Cluster Metadata Service (CMS) component of the
disaggregated storage stack (PALI). The page server already carries
reasonable LOGV2 coverage; this audit focuses on CMS, where gaps are
most likely to hurt incident response. No code changes are proposed here
— the deliverable is a structured set of recommendations for follow-up
work.

## 1. Phases of the CMS lifecycle where logs are essential

Each phase below is a natural seam at which production debugging
typically begins ("we lost visibility around X"); each should emit at
least one anchor LOGV2 line.

1. **Startup handshake.** Process bootstrap, configuration load,
   identity assignment, and the first outbound dial to peers. Today
   most failures here surface as a generic "could not connect"; we need
   per-peer attempt outcomes.
2. **Log-server registration.** The CMS-side registration RPC against
   the durable log service. A silent timeout here puts the node into a
   degraded read-only mode that is hard to distinguish from a healthy
   secondary.
3. **Segment registration.** Each segment that the CMS adopts (whether
   newly cut or recovered) should be acknowledged with its UUID,
   generation, owning shard, and acceptance verdict.
4. **Lookup-on-read.** Read-path cache lookups (UUID → segment
   coordinates). Today only failures are logged; cache pressure and
   refresh storms are invisible.
5. **Lookup-on-write.** Write-path lookups that must commit allocation
   decisions. Distinct from read because the policy on miss is
   different (allocate vs. wait).
6. **Failover / leader change.** Both the losing-leadership and
   gaining-leadership transitions. Today only the *gain* side is
   reliably logged.
7. **Shutdown.** Graceful and crash-induced. We need a clear last-gasp
   line that distinguishes "I drained" from "I was killed mid-flight".

## 2. Five highest-value missing log categories

(a) **Connection establishment per (peer-id, attempt#, outcome).** Today
    we see one aggregate "connection failed" message; we cannot tell
    whether one peer is flapping or many are unreachable. Recommended
    granularity: one line per dial attempt with
    `peer`, `attempt`, `outcome`, `latencyMillis`, `errorCode`.

(b) **Segment-registration accepted / rejected with reason.** Currently
    we log on accept but reject paths fall through to a generic
    assertion. Each reject should name the reason
    (`stale_generation`, `unknown_shard`, `quota_exceeded`, etc.) so
    SREs can correlate against page-server logs.

(c) **Lookup-cache hit / miss / refresh per UUID.** A sampled,
    rate-limited line per cache outcome — not every read — gated on a
    `cmsCacheTraceSampleRate` knob. Production debugging of cold-cache
    storms today requires attaching a debugger.

(d) **Failure-mode triple: first-failure, recovery-attempt, give-up.**
    Many CMS operations retry transparently. Today only the final
    give-up emits a log line; the first failure (root cause) and the
    recovery attempts (cost) are invisible. Each of the three events
    should emit at a distinct LOGV2 id so they can be selected
    independently in a log query.

(e) **Per-target retry budget exhaustion.** When a circuit breaker /
    retry budget for a given peer or segment trips, log it once on
    transition (not on every subsequent rejected call). Pair with a
    matching "budget restored" line so the duration of the outage is
    measurable from logs alone.

## 3. Structured-attribute recommendations per phase

LOGV2 attribute names should be stable across phases so log queries
compose:

- **Startup handshake:** `nodeId`, `cmsRole`, `configVersion`,
  `peerCount`, `bootDurationMillis`.
- **Log-server registration:** `logServerEndpoint`, `registrationId`,
  `attempt`, `outcome`, `latencyMillis`, `errorCode`.
- **Segment registration:** `segmentUuid`, `generation`, `ownerShardId`,
  `verdict`, `rejectReason`, `bytesAccepted`.
- **Lookup-on-read / on-write:** `segmentUuid`, `cacheOutcome` (hit |
  miss | refresh | negative), `lookupLatencyMicros`, `callerOp`
  (read | write | compact), `sampled` (bool).
- **Failover:** `previousLeader`, `newLeader`, `term`,
  `transitionReason`, `pendingOpsDrained`.
- **Shutdown:** `shutdownReason` (graceful | signal | assert),
  `inflightOps`, `lastCommittedSegment`.

Reusing `segmentUuid` and `peer` across all phases means a single
`{segmentUuid: "..."}` log query reconstructs a segment's entire CMS
history.

## 4. Performance considerations

Not every recommendation above should be always-on:

- **Always-log (per event):** startup, log-server registration,
  segment-registration accept/reject, failover, shutdown, retry-budget
  exhaustion transitions. Volume is bounded by topology events.
- **Always-log (per event) but coarse:** failure-mode triple. Bounded
  by failure rate; if a node is healthy this costs nothing.
- **Slow-log threshold gated:** lookup-on-read and lookup-on-write
  outcomes. Default to logging only when
  `lookupLatencyMicros > slowLookupThresholdMicros`
  (suggested default: 10ms, matching the operation profiler
  convention). Cache *misses* should be logged independently of
  latency, since a fast miss is still diagnostic.
- **Sampled:** cache hit lines under
  `cmsCacheTraceSampleRate` (default 0 in production, 1 in test).
  Without sampling, a hot read path emits gigabytes of LOGV2 per hour.
- **Rate-limited:** retry attempts. Use the existing
  `logAndBackoff` helper or equivalent to cap to N lines per minute per
  (peer, error-code).

All recommendations stay within the existing LOGV2 + structured-attr
contract; none require new infrastructure.
