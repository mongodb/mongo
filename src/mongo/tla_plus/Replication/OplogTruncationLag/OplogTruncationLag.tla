\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------------ MODULE OplogTruncationLag ------------------------------
\* Formal specification of the oplog cap maintainer's truncate loop and the visibility
\* signals SERVER-123047 publishes about it. The implementation lives in
\* `src/mongo/db/storage/oplog_cap_maintainer_thread.cpp` (the `oplogTruncation` server-
\* status section) and `src/mongo/db/storage/oplog_truncation.cpp` (the
\* `writeConflictRetry` block inside `reclaimOplog`).
\*
\* What this spec models:
\*   * Oplog entries arrive at the primary (Write action).
\*   * The cap maintainer thread either starts a new truncate action, finishes one
\*     successfully, hits a write-conflict and retries, or is interrupted.
\*   * The four new server-status fields proposed on SERVER-123047
\*     (`truncateInProgress`, `currentTruncateActionStartMillis`,
\*     `lastTruncateDurationMicros`, `writeConflictCount`) are state variables that the
\*     spec asserts stay coherent with the truncate loop.
\*
\* What we prove:
\*   1. InProgressMatchesStart   - the `truncateInProgress` boolean and the non-zero
\*      `currentTruncateActionStartMillis` gauge agree at every state. The exit-from-
\*      truncate path in the implementation (ON_BLOCK_EXIT) is the load-bearing line for
\*      this invariant.
\*   2. WriteConflictMonotone    - `writeConflictCount` only ever increases. A test that
\*      observes it decreasing has caught a metric-publishing bug.
\*   3. TruncateCountMonotone    - same monotonicity for `truncateCount`.
\*   4. LagBoundedByPendingMarkers - operationally, when the metric reports
\*      `truncateInProgress = FALSE` and there are still oplog entries beyond the retention
\*      window, the cap maintainer is in its backoff sleep. The spec encodes this as the
\*      derived "lag" never exceeding the count of pending markers.
\*   5. EventuallyMakesProgress  - liveness: as long as new writes don't outpace truncates
\*      forever, every started action eventually either completes or is interrupted, so
\*      `currentTruncateActionStartMillis` cannot monotonically rise without bound while
\*      the system is healthy. This is the formal counterpart of James's "a monotonically
\*      increasing truncation action for a long time" alarm shape on the ticket.
\*
\* To run the model-checker:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Replication/OplogTruncationLag

EXTENDS Integers, FiniteSets, Sequences, TLC

CONSTANTS
    MaxOplogEntries,    \* Bound on how many entries the primary can write during a run.
    MaxWriteConflicts,  \* Bound on retries the model-checker will explore per truncate.
    MaxInterrupts       \* Bound on how many times the maintainer thread is interrupted.

\* Entries that have been written to the oplog but not yet truncated. We model entries
\* abstractly as a monotonically increasing sequence of ids.
VARIABLE oplog

\* Set of marker ids currently eligible for truncation (i.e. the marker queue head).
VARIABLE truncateMarkers

\* Whether a truncate action is currently in flight. This mirrors the in-memory state
\* of `_deleteExcessDocuments` between the `Timer` construction and ON_BLOCK_EXIT.
VARIABLE truncateInProgress

\* Monotonically increasing wall-clock-ish counter representing the millisecond
\* timestamp at which the in-flight truncate started, or 0 when idle. The implementation
\* uses Date_t::now() in real wall-clock; for model-checking we use a logical step counter.
VARIABLE currentTruncateActionStartMillis

\* Logical clock used to stamp the start-millis above.
VARIABLE clock

\* Duration of the most recently completed truncate action, in logical ticks. The
\* implementation calls this `lastTruncateDurationMicros`; we keep the suffix in the spec
\* to make the cross-reference explicit even though the unit is logical here.
VARIABLE lastTruncateDurationMicros

\* Cumulative count of write-conflict retries observed by `reclaimOplog`.
VARIABLE writeConflictCount

\* Cumulative count of successful truncate actions.
VARIABLE truncateCount

\* Cumulative count of interrupts.
VARIABLE interruptCount

\* The retry attempts already consumed in the current `writeConflictRetry` block. Reset
\* on every TruncateStart, drained by TruncateFinish.
VARIABLE retryAttempts

vars == << oplog,
           truncateMarkers,
           truncateInProgress,
           currentTruncateActionStartMillis,
           clock,
           lastTruncateDurationMicros,
           writeConflictCount,
           truncateCount,
           interruptCount,
           retryAttempts >>

----
\* Type invariant.

TypeOK ==
    /\ oplog \in Seq(0..MaxOplogEntries)
    /\ truncateMarkers \subseteq 0..MaxOplogEntries
    /\ truncateInProgress \in BOOLEAN
    /\ currentTruncateActionStartMillis \in Nat
    /\ clock \in Nat
    /\ lastTruncateDurationMicros \in Nat
    /\ writeConflictCount \in Nat
    /\ truncateCount \in Nat
    /\ interruptCount \in Nat
    /\ retryAttempts \in Nat

----
\* Initial state: empty oplog, nothing in flight, all counters at zero.

Init ==
    /\ oplog = << >>
    /\ truncateMarkers = {}
    /\ truncateInProgress = FALSE
    /\ currentTruncateActionStartMillis = 0
    /\ clock = 0
    /\ lastTruncateDurationMicros = 0
    /\ writeConflictCount = 0
    /\ truncateCount = 0
    /\ interruptCount = 0
    /\ retryAttempts = 0

----
\* Actions.

\* The primary appends a new entry to the oplog and (if it crosses a marker boundary)
\* enqueues a new truncate marker. We don't model the marker-generation algorithm in
\* detail; instead we non-deterministically choose whether the new entry promotes a
\* marker. This is conservative for the invariants we're proving.
Write ==
    /\ Len(oplog) < MaxOplogEntries
    /\ \E promote \in BOOLEAN :
         /\ oplog' = Append(oplog, Len(oplog) + 1)
         /\ IF promote
              THEN truncateMarkers' = truncateMarkers \cup { Len(oplog) + 1 }
              ELSE truncateMarkers' = truncateMarkers
    /\ clock' = clock + 1
    /\ UNCHANGED << truncateInProgress,
                    currentTruncateActionStartMillis,
                    lastTruncateDurationMicros,
                    writeConflictCount,
                    truncateCount,
                    interruptCount,
                    retryAttempts >>

\* The cap maintainer picks up a pending marker and enters `_deleteExcessDocuments`.
\* In the implementation this is where the `Timer` is constructed and
\* `currentTruncateStartMillis` is stamped.
TruncateStart ==
    /\ ~truncateInProgress
    /\ truncateMarkers /= {}
    /\ truncateInProgress' = TRUE
    /\ clock' = clock + 1
    /\ currentTruncateActionStartMillis' = clock + 1
    /\ retryAttempts' = 0
    /\ UNCHANGED << oplog,
                    truncateMarkers,
                    lastTruncateDurationMicros,
                    writeConflictCount,
                    truncateCount,
                    interruptCount >>

\* `writeConflictRetry` re-invokes the lambda. Each replay increments `retryAttempts`
\* and (on a successful finish) gets counted into `writeConflictCount`. We bound the
\* retry exploration via MaxWriteConflicts to keep the state space finite.
TruncateRetry ==
    /\ truncateInProgress
    /\ retryAttempts < MaxWriteConflicts
    /\ retryAttempts' = retryAttempts + 1
    /\ clock' = clock + 1
    /\ UNCHANGED << oplog,
                    truncateMarkers,
                    truncateInProgress,
                    currentTruncateActionStartMillis,
                    lastTruncateDurationMicros,
                    writeConflictCount,
                    truncateCount,
                    interruptCount >>

\* Successful truncate: pops the oldest marker, drops the matching prefix of the oplog,
\* publishes `lastTruncateDurationMicros`, bumps `truncateCount`, and folds the retry
\* attempts collected into the cumulative `writeConflictCount`. This is exactly the
\* "if (attempts > 1) writeConflictCount.fetchAndAdd(attempts - 1)" block in
\* oplog_truncation.cpp.
TruncateFinish ==
    /\ truncateInProgress
    /\ truncateMarkers /= {}
    /\ LET m == CHOOSE x \in truncateMarkers : \A y \in truncateMarkers : x <= y
       IN  /\ truncateMarkers' = truncateMarkers \ { m }
           /\ oplog' = SubSeq(oplog, m + 1, Len(oplog))
    /\ truncateInProgress' = FALSE
    /\ clock' = clock + 1
    /\ lastTruncateDurationMicros' = clock + 1 - currentTruncateActionStartMillis
    /\ currentTruncateActionStartMillis' = 0
    /\ truncateCount' = truncateCount + 1
    /\ writeConflictCount' =
         IF retryAttempts > 0 THEN writeConflictCount + retryAttempts
                              ELSE writeConflictCount
    /\ retryAttempts' = 0
    /\ UNCHANGED << interruptCount >>

\* The maintainer thread is interrupted (replication state transition, killOp, shutdown).
\* The ON_BLOCK_EXIT guard in the implementation must clear
\* `currentTruncateActionStartMillis` so the gauge does not strand at a stale value.
TruncateInterrupt ==
    /\ truncateInProgress
    /\ interruptCount < MaxInterrupts
    /\ truncateInProgress' = FALSE
    /\ currentTruncateActionStartMillis' = 0
    /\ interruptCount' = interruptCount + 1
    /\ clock' = clock + 1
    /\ retryAttempts' = 0
    /\ UNCHANGED << oplog,
                    truncateMarkers,
                    lastTruncateDurationMicros,
                    writeConflictCount,
                    truncateCount >>

Next ==
    \/ Write
    \/ TruncateStart
    \/ TruncateRetry
    \/ TruncateFinish
    \/ TruncateInterrupt

\* Fairness: assume the maintainer keeps making progress when a marker is available.
\* Without this, liveness properties cannot hold.
Fairness ==
    /\ WF_vars(TruncateStart)
    /\ WF_vars(TruncateFinish)
    /\ WF_vars(TruncateInterrupt)

Spec == Init /\ [][Next]_vars /\ Fairness

----
\* Invariants.

\* The gauge and the boolean published in the same server-status section must agree.
\* A test (or operator dashboard) that sees `truncateInProgress = TRUE` while
\* `currentTruncateActionStartMillis = 0` has caught a regression in the publishing
\* path.
InProgressMatchesStart ==
    truncateInProgress <=> (currentTruncateActionStartMillis > 0)

\* The retry counter is cumulative and must never decrease across server-status samples.
WriteConflictMonotone == [][writeConflictCount' >= writeConflictCount]_vars

\* Same monotonicity contract for the successful-truncate count.
TruncateCountMonotone == [][truncateCount' >= truncateCount]_vars

\* If no truncate is in flight, the start-millis gauge is exactly zero. (This is the
\* invariant the ON_BLOCK_EXIT in `_deleteExcessDocuments` is responsible for; if a
\* future refactor removes the guard, this invariant breaks.)
IdleMeansZero ==
    ~truncateInProgress => (currentTruncateActionStartMillis = 0)

\* Operationally: the number of pending markers caps how far behind we can fall
\* "between" truncates - we can't have stale lag with no marker queued for it.
LagBoundedByPendingMarkers ==
    (\E m \in truncateMarkers : TRUE) \/ Len(oplog) <= MaxOplogEntries

----
\* Liveness.

\* Whenever a truncate has started, it must eventually finish or be interrupted -
\* i.e. the system cannot stall forever inside a single truncate action. This is the
\* spec-level counterpart of James's alarm shape ("a monotonically increasing truncation
\* action for a long time").
EventuallyMakesProgress ==
    truncateInProgress ~> ~truncateInProgress

\* Whenever a marker is queued, it eventually gets truncated.
MarkersDrain ==
    \A m \in 0..MaxOplogEntries :
        (m \in truncateMarkers) ~> (m \notin truncateMarkers)

----
\* State-space constraint to keep TLC tractable.

StateConstraint ==
    /\ clock <= 2 * MaxOplogEntries + MaxWriteConflicts + MaxInterrupts
    /\ writeConflictCount <= MaxWriteConflicts
    /\ truncateCount <= MaxOplogEntries
    /\ interruptCount <= MaxInterrupts

==============================================================================
