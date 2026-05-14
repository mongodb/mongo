# SLS-5308: Metastability Guardrails for Disaggregated Storage

**Status:** Design
**Parent epic:** SLS-2891 (Evaluate Metastable failure risk in Disaggregated Storage)
**Owning team:** SLS Availability
**Formal companion:** `src/mongo/tla_plus/Disagg/MetastabilityGuardrails/`

## What metastability is

A *metastable* failure is one where a transient trigger pushes the
system across a threshold into a degraded state, and the system stays
in that degraded state after the trigger is gone. Bronson, Aghayev,
Charapko, and Zhu's 2021 HotOS paper framed this as two basins
separated by a stress-amplification feedback loop: once the system
crosses the ridge, the feedback (typically retries) keeps load above
the level that would otherwise allow recovery, even when the original
disturbance has cleared.

In disaggregated storage the blast radius is wide: a single metastable
event in the PageServer fan-out or AppendLog write pipeline can pin
canaries red for the entire duration of a manual intervention.
SLS-5308 requires any red interval to stay inside 30 s p95 / 90 s p99
across compound-fault drills, which is unreachable without guardrails.

## Four known disagg failure modes and their guardrails

1. **Retry storms on PageServer reads.** A brief PageServer stall
   pushes per-page reads past the client read timeout. Each timeout
   enqueues a fresh attempt; the admission queue saturates; further
   reads see worse queueing latency and time out faster.
   *Guardrail:* **Load shedding via tarpit**. Once the admission queue
   crosses a high-water mark, new offers are fast-rejected with a small
   latency penalty, propagating backpressure without adding in-flight
   work. Modeled as the `ClientOffer` shed branch under
   `GuardrailsEnabled /\ AboveHighWater`.

2. **AppendLog timeout cascades.** The AppendLog write pipeline issues
   parallel appends to a quorum of log nodes; one slow node turns the
   tail into timeouts. Retried appends land in lockstep with the next
   batch and re-collide.
   *Guardrail:* **Jitter on retry backoff**. Per-attempt randomised
   delay desynchronises retries so they no longer arrive as a phased
   burst. In the spec, jitter shows up as `ClockTick` interleaved
   non-deterministically with retries.

3. **CMS lookup loops.** Clients re-enter the Cluster Metadata Service
   lookup path whenever a routing entry stales out. A brief CMS outage
   causes every client to re-lookup at once; slow replies are
   interpreted as stale answers and re-lookup repeats.
   *Guardrail:* **Per-target circuit breaker**. After a threshold of
   consecutive failures, the breaker opens and refuses dispatch until a
   cool-down elapses, then sends a single half-open probe. Modeled as
   `breakerState[tgt]` transitioning `CLOSED -> OPEN -> HALFOPEN ->
   CLOSED` driven by `breakerFailureRun[tgt]` and `BreakerCoolDown`.

4. **Log truncation backpressure.** When the truncation worker falls
   behind, write admission slows; writers retry "stuck" appends, which
   adds uncommitted entries, which makes truncation slower still.
   *Guardrail:* **Queue depth cap with backpressure**. A hard
   `QueueCapacity` is enforced at admission and signaled synchronously
   to writers. Modeled by the `QueueDepth < QueueCapacity` precondition
   on `ClientOffer` and the `QueueBounded` invariant.

## Why a formal spec, given the maxim DES already exists

MongoDB Research's maxim discrete-event simulator (SLS-2891) models
timing distributions for these subsystems and produces quantitative
latency curves. Maxim answers *"how bad does it get and when does it
recover?"* across a wide parameter sweep. What it cannot answer alone
is the qualitative liveness question — does the system *ever* recover
after the trigger is removed, for *all* interleavings of retry,
dispatch, and breaker decisions? That is what TLC checks, and what the
spec in `src/mongo/tla_plus/Disagg/MetastabilityGuardrails/`
discharges.

The two tools are complementary. Maxim drives compound-fault drill
parameters and produces SLO evidence. The TLA+ spec pins the guardrail
set: the bug `.cfg` produces a liveness counter-example where the
queue stays full forever after the trigger clears; the healthy `.cfg`
proves `EventuallyReturnsToHealthy` under guardrails-on. If a future
code change removes one of the four guardrails, the healthy `.cfg`
starts failing under `./model-check.sh Disagg/MetastabilityGuardrails`.

## Compound-fault drill checklist

Drills combine at least two faults and confirm guardrails hold:

- PageServer 5xx burst + AppendLog single-node stall
- CMS slow-reply + reconfig in-progress
- Truncation lag + sustained 90th-percentile traffic
- Network partition restored mid-flight with retry-in-flight backlog

Per drill, capture queue depth time series, breaker state transitions
per target, shed-rate, and trigger-cleared → canary-green time.
Success bar: red interval ≤ 30 s p95 / ≤ 90 s p99 per ticket scope.

## References

- Bronson et al., "Metastable Failures in Distributed Systems", HotOS 2021
- SLS-2891, parent epic on metastable-failure-risk evaluation
- maxim DES framework (MongoDB Research)
- TLA+ spec at `src/mongo/tla_plus/Disagg/MetastabilityGuardrails/`
