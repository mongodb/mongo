# PALI Failover Telemetry Audit — SERVER-115688

**Scope:** observability audit for PALI (disaggregated Page-And-Log Infrastructure) failover. The
ticket asks that operators (a) identify when a `mongod` is failing over out of its preferred zone
and (b) identify which page/log servers it is talking to during and after the transition. This
doc enumerates the failover state machine, lists the observability surfaces expected at each
transition, and names the gaps the companion jstest pins.

## Failover state machine

A PALI failover walks five transitions. Each has its own SLO and its own diagnostic question.

1. **`primary-loss-detected`** — heartbeat / lease deadline elapsed. *When did we learn, and which signal told us?*
2. **`quorum-election`** — node calls `replSetRequestVotes`, collects votes. *Who voted, in what term, at what op-time?*
3. **`catchup`** — newly elected primary drains the catchup window. *How long, against which sync source?*
4. **`standby-exit`** — node leaves `SECONDARY`, clears the no-op writer, takes RSTL X. *How long did RSTL take, what held it?*
5. **`become-primary`** — first user write accepted; PALI rebinds page/log server affinity. *When did the first write land, and to which zone?*

## Required telemetry per transition

**Bold** entries are missing or partial; the companion jstest probes for them.

**1. `primary-loss-detected`** — `LOGV2 "Heartbeat reported primary unreachable"` present in
`replication_coordinator_impl_heartbeat.cpp`. serverStatus exposes only
`electionMetrics.numStepDownsCausedByHigherTerm`; **no counter for heartbeat-timeout-driven loss**.
FTDC captures `members[].lastHeartbeatRecv`, coarse.

**2. `quorum-election`** — `LOGV2` from `VoteRequester` present. serverStatus
`electionMetrics.{electionTimeout,stepUpCmd,priorityTakeover,catchUpTakeover}` counters present.
FTDC `electionCandidateMetrics.{electionTerm,numVotesNeeded,priorityAtElection}` present.

**3. `catchup`** — `"Caught up to the latest known optime"` / `"Catchup timed out"` log lines
present. serverStatus has counts (`numCatchUps`, `numCatchUpsSucceeded`, `numCatchUpsTimedOut`).
**Gap: no per-election `catchupDurationMillis` histogram.** Operators cannot distinguish a 50ms
catchup from a 4900ms catchup. PALI catchup against a disaggregated log tier is I/O-bound in a way
classical replica-set catchup is not.

**4. `standby-exit`** — `"Transition to PRIMARY complete"` exists, but **RSTL-X acquisition wait
is not logged as a structured attr** (rolled into slow-op at >100ms). **serverStatus has no
`preStepUpWaitMillis`.** Interval between "won election" and "RSTL acquired" is invisible unless
slow-op fires. FTDC: no dedicated section.

**5. `become-primary`** — transition-complete LOGV2 present. PALI side **does not log the bound
page-server zone after transition** (Aaron's ticket comment requests a `LOG_EVERY_N` of
out-of-zone request counts). **No `paliZoneAffinity` serverStatus block** carrying
`{currentZone, pageServerEndpoints[], logServerEndpoints[], outOfZoneRequests24h}`. **No
`paliFailoverLatency` FTDC section** carrying the four step-up latencies below.

## Called-out gaps

Four latencies are the load-bearing missing measurements. Each maps to one PALI failover SLO and
each is asserted in `jstests/replsets/pali_failover_telemetry_gates.js`. The jstest fails today;
it documents the deliverable in executable form.

1. **`preStepUpWaitMillis`** — "election won" to "RSTL X acquired". Says whether step-up is
   bottlenecked on long-running reads or on the no-op writer.
2. **`catchupDurationMillis`** — wall time of the catchup phase. Distinguishes "already up to
   date" from "waited 4s on an out-of-zone log server".
3. **`standbyToPrimaryMillis`** — "RSTL acquired" to "transition callback returned". Catches
   drainage / op-observer cost.
4. **`postStepUpFirstWriteMillis`** — "transition complete" to "first user write applied".
   The user-visible failover SLI; PALI re-binding to a remote zone is the suspected regression source.

Two non-latency gaps round out the audit:

5. **Zone affinity log line** — `LOG_EVERY_N(1000)` of
   `{currentZone, outOfZoneReadsPct, outOfZonePageServerCount}` so operators see "we failed over
   and are now reading 30% out of zone" without per-request log scraping (Aaron's comment).
6. **Per-page-server-endpoint counter** — `paliPageServerRequests.{endpoint,inFlight,errors,p99LatencyMillis}`
   so a known-unhealthy page server can be confirmed as the source of poor primary performance.

## Recommended next steps

1. Add the four `*Millis` fields to `replication_metrics.idl` (or a new `paliFailoverMetrics.idl`)
   and emit from existing step-up / catchup callsites.
2. Add `paliZoneAffinity` serverStatus section under `src/mongo/db/disagg/pali/` and wire into FTDC.
3. Land the `LOG_EVERY_N` zone-affinity log per Aaron's comment.
4. Convert the jstest from "documents gaps" to "regression gate" once metrics land.

The audit ships no production code — only this doc and the executable gap-list — so reviewers can
scope the implementation ticket against a concrete metric checklist.
