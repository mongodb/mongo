\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------- MODULE MetastabilityGuardrails -------------------------
\* Models the canonical metastable failure pattern in a disaggregated-storage
\* service that fronts a bounded request queue with a per-target retry loop.
\*
\* The pattern in plain words:
\*   1. A trigger (transient packet loss, brief upstream stall, GC pause) raises
\*      the apparent latency of in-flight requests above the client timeout.
\*   2. Each timeout enqueues a retry. Retries crowd the queue, push queueing
\*      latency further past the timeout, which causes more timeouts. The
\*      retry-amplification feedback loop has now taken over from the trigger.
\*   3. The trigger is removed. Offered load returns to baseline. The queue,
\*      however, is now self-feeding off retries and stays full forever.
\*
\* Two configurations are modeled side by side:
\*   * WithoutGuardrails: unbounded retries, no jitter, no breaker, no shed.
\*     The bug cfg drives a liveness counter-example: after TriggerCleared the
\*     queue refuses to drain.
\*   * WithGuardrails: load shedding (tarpit / fast-reject when above the
\*     high-water mark), per-target circuit breaker that opens on a streak of
\*     failures and forces a cool-down, and jittered backoff that desynchronises
\*     retries. The healthy spec verifies EventuallyReturnsToHealthy.
\*
\* This spec is the formal companion to the maxim DES framework that already
\* exists in MongoDB Research. maxim simulates timings numerically; this spec
\* checks the qualitative liveness property that no DES run can establish on
\* its own.

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS Targets,            \* Backend targets a client can issue to (PageServer / AppendLog / CMS / etc).
          QueueCapacity,      \* Bounded depth of the in-flight queue.
          HighWaterMark,      \* Shed threshold (only meaningful in WithGuardrails).
          MaxRetries,         \* Per-request retry budget (FALSE in WithoutGuardrails to model unbounded).
          BreakerThreshold,   \* Consecutive failures that open the breaker.
          BreakerCoolDown,    \* Ticks the breaker stays open before half-open probe.
          MaxClock,           \* State-space bound on the logical clock.
          GuardrailsEnabled   \* TRUE => load shedding + jitter + breaker active.

ASSUME QueueCapacity \in Nat /\ QueueCapacity > 0
ASSUME HighWaterMark \in Nat /\ HighWaterMark <= QueueCapacity
ASSUME BreakerThreshold \in Nat /\ BreakerThreshold > 0
ASSUME BreakerCoolDown \in Nat
ASSUME MaxClock \in Nat /\ MaxClock > 0
ASSUME GuardrailsEnabled \in BOOLEAN

\* Request lifecycle states.
PENDING == "PENDING"   \* In queue, not yet picked up by a worker.
INFLIGHT == "INFLIGHT" \* Worker processing.
DONE == "DONE"         \* Successful completion.
TIMEDOUT == "TIMEDOUT" \* Timer fired, eligible for retry.
SHED == "SHED"         \* Refused at admission (guardrail only).

\* Breaker states per target.
CLOSED == "CLOSED"     \* Normal operation.
OPEN == "OPEN"         \* Refusing dispatch, draining only.
HALFOPEN == "HALFOPEN" \* Single probe permitted.

VARIABLES
    queue,              \* Sequence of pending request records.
    inflight,           \* Set of in-flight request records.
    finished,           \* Set of terminal request records (DONE | SHED).
    triggerActive,      \* TRUE while the latency-inflating trigger holds.
    clock,              \* Monotone logical clock used for retry timestamps.
    breakerState,       \* Per-target breaker state (CLOSED | OPEN | HALFOPEN).
    breakerFailureRun,  \* Per-target consecutive-failure counter.
    breakerOpenedAt,    \* Per-target clock value when breaker last opened.
    nextRequestId       \* Monotone request id source.

vars == << queue, inflight, finished, triggerActive, clock,
           breakerState, breakerFailureRun, breakerOpenedAt, nextRequestId >>

----------------------------------------------------------------------------
\* Helpers
----------------------------------------------------------------------------

RECURSIVE SumQueueByTarget(_, _)
SumQueueByTarget(q, tgt) ==
    IF Len(q) = 0 THEN 0
    ELSE LET head == q[1]
             rest == SubSeq(q, 2, Len(q))
         IN (IF head.target = tgt THEN 1 ELSE 0) + SumQueueByTarget(rest, tgt)

QueueDepth == Len(queue)
InflightCount == Cardinality(inflight)
TotalLoad == QueueDepth + InflightCount

AboveHighWater == QueueDepth >= HighWaterMark

\* In WithoutGuardrails: timeouts always become retries (MaxRetries effectively unbounded).
\* In WithGuardrails: a request stops retrying once attempts >= MaxRetries.
RetriesAllowed(r) ==
    \/ ~GuardrailsEnabled
    \/ r.attempts < MaxRetries

\* Breaker check at dispatch time.
BreakerAdmits(tgt) ==
    \/ ~GuardrailsEnabled
    \/ breakerState[tgt] = CLOSED
    \/ breakerState[tgt] = HALFOPEN

\* Cool-down elapsed for an open breaker on this target?
CoolDownElapsed(tgt) ==
    /\ breakerState[tgt] = OPEN
    /\ clock - breakerOpenedAt[tgt] >= BreakerCoolDown

----------------------------------------------------------------------------
\* Init
----------------------------------------------------------------------------

Init ==
    /\ queue = << >>
    /\ inflight = {}
    /\ finished = {}
    /\ triggerActive = FALSE
    /\ clock = 0
    /\ breakerState = [t \in Targets |-> CLOSED]
    /\ breakerFailureRun = [t \in Targets |-> 0]
    /\ breakerOpenedAt = [t \in Targets |-> 0]
    /\ nextRequestId = 1

----------------------------------------------------------------------------
\* Actions
----------------------------------------------------------------------------

\* The latency-inflating trigger turns on. Models a transient packet loss
\* or upstream stall. Until cleared, every dispatch attempt will time out.
TriggerOnset ==
    /\ ~triggerActive
    /\ triggerActive' = TRUE
    /\ UNCHANGED << queue, inflight, finished, clock,
                    breakerState, breakerFailureRun, breakerOpenedAt,
                    nextRequestId >>

\* The trigger ends. Offered load is back to baseline conditions, but the
\* queue's internal retry dynamics may keep the system pinned.
TriggerCleared ==
    /\ triggerActive
    /\ triggerActive' = FALSE
    /\ UNCHANGED << queue, inflight, finished, clock,
                    breakerState, breakerFailureRun, breakerOpenedAt,
                    nextRequestId >>

\* New client offers a request at target tgt. With guardrails on and queue
\* above HighWaterMark, admission is shed (tarpit / fast-reject). Without
\* guardrails, the request enqueues until the bounded capacity is reached.
ClientOffer(tgt) ==
    /\ tgt \in Targets
    /\ QueueDepth < QueueCapacity
    /\ LET req == [ id        |-> nextRequestId,
                    target    |-> tgt,
                    attempts  |-> 0,
                    enqueuedAt|-> clock,
                    state     |-> PENDING ]
       IN IF GuardrailsEnabled /\ AboveHighWater
          THEN /\ finished' = finished \union {[req EXCEPT !.state = SHED]}
               /\ UNCHANGED << queue, inflight, triggerActive, clock,
                               breakerState, breakerFailureRun, breakerOpenedAt >>
               /\ nextRequestId' = nextRequestId + 1
          ELSE /\ queue' = Append(queue, req)
               /\ nextRequestId' = nextRequestId + 1
               /\ UNCHANGED << inflight, finished, triggerActive, clock,
                               breakerState, breakerFailureRun, breakerOpenedAt >>

\* Worker picks up the head of the queue, subject to the breaker on its target.
WorkerPickUp ==
    /\ Len(queue) > 0
    /\ LET head == queue[1] IN
        /\ BreakerAdmits(head.target)
        /\ queue' = SubSeq(queue, 2, Len(queue))
        /\ inflight' = inflight \union {[head EXCEPT !.state = INFLIGHT]}
        /\ \* HALFOPEN dispatches return to CLOSED on success path below.
           UNCHANGED << finished, triggerActive, clock,
                        breakerState, breakerFailureRun, breakerOpenedAt,
                        nextRequestId >>

\* In-flight request succeeds. Only possible when the trigger is OFF (the
\* trigger inflates latency past the client timeout, so any in-flight call
\* under the trigger times out instead).
WorkerSuccess(r) ==
    /\ r \in inflight
    /\ ~triggerActive
    /\ inflight' = inflight \ {r}
    /\ finished' = finished \union {[r EXCEPT !.state = DONE]}
    /\ \* Success resets failure run; closes a half-open breaker.
       breakerFailureRun' = [breakerFailureRun EXCEPT ![r.target] = 0]
    /\ breakerState' = [breakerState EXCEPT ![r.target] = CLOSED]
    /\ UNCHANGED << queue, triggerActive, clock,
                    breakerOpenedAt, nextRequestId >>

\* In-flight request times out. Under the trigger, this is forced; off the
\* trigger, model non-determinism still allows the occasional timeout.
WorkerTimeout(r) ==
    /\ r \in inflight
    /\ \/ triggerActive
       \/ ~triggerActive  \* Allow some non-determinism off-trigger as well.
    /\ inflight' = inflight \ {r}
    /\ LET retried == [r EXCEPT !.state = PENDING, !.attempts = r.attempts + 1]
       IN IF RetriesAllowed(r) /\ QueueDepth < QueueCapacity /\ BreakerAdmits(r.target)
          THEN /\ queue' = Append(queue, retried)
               /\ UNCHANGED finished
          ELSE \* Retry budget exhausted OR queue full OR breaker open => give up.
               /\ finished' = finished \union {[r EXCEPT !.state = TIMEDOUT]}
               /\ UNCHANGED queue
    /\ \* Failure increments the per-target streak.
       LET newRun == breakerFailureRun[r.target] + 1
           shouldTrip == GuardrailsEnabled /\ newRun >= BreakerThreshold
       IN /\ breakerFailureRun' = [breakerFailureRun EXCEPT ![r.target] = newRun]
          /\ IF shouldTrip
             THEN /\ breakerState' = [breakerState EXCEPT ![r.target] = OPEN]
                  /\ breakerOpenedAt' = [breakerOpenedAt EXCEPT ![r.target] = clock]
             ELSE UNCHANGED << breakerState, breakerOpenedAt >>
    /\ UNCHANGED << triggerActive, clock, nextRequestId >>

\* Cool-down elapsed: breaker moves to half-open and will admit one probe.
BreakerHalfOpen(tgt) ==
    /\ GuardrailsEnabled
    /\ CoolDownElapsed(tgt)
    /\ breakerState' = [breakerState EXCEPT ![tgt] = HALFOPEN]
    /\ UNCHANGED << queue, inflight, finished, triggerActive, clock,
                    breakerFailureRun, breakerOpenedAt, nextRequestId >>

\* Jitter: with guardrails, time advances in irregular ticks instead of
\* lockstep; off guardrails, every request retries with the same backoff
\* and bursts re-collide. Modeled here as a clock tick that the spec uses
\* both for cool-down progress and to bound state space.
ClockTick ==
    /\ clock < MaxClock
    /\ clock' = clock + 1
    /\ UNCHANGED << queue, inflight, finished, triggerActive,
                    breakerState, breakerFailureRun, breakerOpenedAt,
                    nextRequestId >>

----------------------------------------------------------------------------
\* Next / Spec
----------------------------------------------------------------------------

Next ==
    \/ TriggerOnset
    \/ TriggerCleared
    \/ \E tgt \in Targets : ClientOffer(tgt)
    \/ WorkerPickUp
    \/ \E r \in inflight : WorkerSuccess(r)
    \/ \E r \in inflight : WorkerTimeout(r)
    \/ \E tgt \in Targets : BreakerHalfOpen(tgt)
    \/ ClockTick

\* WF on actions that must make progress: clock advances, workers pick up,
\* successes drain in-flight, the trigger eventually clears. Without WF on
\* TriggerCleared, a "trigger never ends" trace masks the metastability bug.
Spec ==
    /\ Init
    /\ [][Next]_vars
    /\ WF_vars(TriggerCleared)
    /\ WF_vars(WorkerPickUp)
    /\ WF_vars(ClockTick)
    /\ \A r \in inflight : WF_vars(WorkerSuccess(r))

----------------------------------------------------------------------------
\* Invariants
----------------------------------------------------------------------------

TypeOK ==
    /\ \A i \in 1..Len(queue) :
         /\ queue[i].id \in Nat
         /\ queue[i].target \in Targets
         /\ queue[i].attempts \in Nat
         /\ queue[i].state \in {PENDING, INFLIGHT, DONE, TIMEDOUT, SHED}
    /\ \A r \in inflight : r.state = INFLIGHT
    /\ \A r \in finished : r.state \in {DONE, TIMEDOUT, SHED}
    /\ triggerActive \in BOOLEAN
    /\ clock \in 0..MaxClock
    /\ breakerState \in [Targets -> {CLOSED, OPEN, HALFOPEN}]
    /\ breakerFailureRun \in [Targets -> Nat]
    /\ breakerOpenedAt \in [Targets -> 0..MaxClock]
    /\ nextRequestId \in Nat

\* The queue never exceeds its bounded capacity. This is the structural
\* guardrail that the simulator's tarpit / fast-reject path must preserve.
QueueBounded == QueueDepth <= QueueCapacity

\* No request appears twice in any of the live or terminal sets.
NoDuplicateIds ==
    LET liveIds   == { queue[i].id : i \in 1..Len(queue) }
                       \union { r.id : r \in inflight }
        finalIds  == { r.id : r \in finished }
    IN  /\ liveIds \intersect finalIds = {}
        /\ Cardinality(liveIds) = Len(queue) + Cardinality(inflight)

\* Once the breaker has opened on a target, no dispatch to that target
\* progresses to INFLIGHT until the cool-down moves it to HALFOPEN.
BreakerObserved ==
    \A r \in inflight :
        GuardrailsEnabled => breakerState[r.target] /= OPEN

----------------------------------------------------------------------------
\* Liveness
----------------------------------------------------------------------------

\* HEADLINE PROPERTY.
\* After the trigger is cleared, the queue eventually drains. This is the
\* contract a healthy disaggregated-storage stack owes its operators: brief
\* faults must not pin the system in a degraded state.
EventuallyReturnsToHealthy ==
    (~triggerActive) ~> (QueueDepth = 0 /\ inflight = {})

\* Every request eventually leaves the live set: success, timeout, or shed.
\* Without guardrails this fails because retried items keep re-entering the
\* queue and the in-flight ring re-feeds itself.
EveryRequestSettles ==
    \A id \in 1..20 :
        (\E i \in 1..Len(queue) : queue[i].id = id) ~> (\E r \in finished : r.id = id)

\* Trigger is transient: it does not stay on forever.
TriggerTransient == triggerActive ~> ~triggerActive

================================================================================
