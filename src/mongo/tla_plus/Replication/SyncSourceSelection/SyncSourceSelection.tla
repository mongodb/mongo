\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------- MODULE SyncSourceSelection -------------------------
\* Formal model of ReplicationCoordinatorImpl::shouldChangeSyncSource and the
\* paired shouldChangeSyncSourceOnError entry point.
\*
\* Background (SERVER-126246): the production code in
\* src/mongo/db/repl/replication_coordinator_impl.cpp returns
\*   - kStopSyncingAndEnqueueLastBatch    when topology says "change source"
\*   - kStopSyncingAndDropLastBatchIfPresent when ping-time says "closer node"
\*   - kContinueSyncing                   otherwise
\* The bug observed in BF-43158 is that the "enqueue last batch" branch is
\* taken unconditionally even when applying the buffered batch would violate
\* a stable-timestamp / commit-point invariant on the new sync source.  In
\* that case the buffered batch must be DROPPED, not enqueued, because the
\* batch's commit timestamp would precede the new source's stable timestamp.
\*
\* This spec encodes:
\*   1) the three-action decision FSM,
\*   2) the predicates the topology coordinator consults
\*      (topologyWantsChange, pingTimeWantsChange, lastBatchEnqueueable),
\*   3) the proposed fix: kStopSyncingAndEnqueueLastBatch may be returned only
\*      when lastBatchEnqueueable holds; otherwise we downgrade to
\*      kStopSyncingAndDropLastBatchIfPresent.
\*
\* To run the model-checker, edit MCSyncSourceSelection.cfg if desired, then:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Replication/SyncSourceSelection/SyncSourceSelection

EXTENDS Integers, FiniteSets, Sequences, TLC

\* The set of replica-set member IDs.
CONSTANT Server

\* The maximum number of decision invocations we model in a single behaviour.
\* Bound keeps the state space finite.
CONSTANT MaxDecisions

\* The set of HostAndPort identifiers we treat as "in config" candidates.
\* A sync source observed at runtime can also be NotInConfig.
CONSTANT NotInConfig

\* The three actions defined by ChangeSyncSourceAction in
\* src/mongo/db/repl/sync_source_selector.h.
Actions == {"kContinueSyncing",
            "kStopSyncingAndDropLastBatchIfPresent",
            "kStopSyncingAndEnqueueLastBatch"}

\* The two entry points: the heartbeat-driven path (with metadata) and the
\* error-driven path (no metadata, restricted check set).
EntryPoints == {"shouldChangeSyncSource", "shouldChangeSyncSourceOnError"}

----
\* Per-decision state observed by each invocation.  Each invocation is a
\* tuple capturing the inputs and the action chosen.  We accumulate them in
\* a sequence so model-checker can express temporal invariants like
\* "no behaviour ever returns enqueueLastBatch when lastBatch is unsafe".

\* The history of decisions produced so far.  Each element:
\*   [ entry           |-> EntryPoints,
\*     currentSource   |-> Server \cup {NotInConfig},
\*     topologyWants   |-> BOOLEAN,
\*     pingTimeWants   |-> BOOLEAN,
\*     forceCleared    |-> BOOLEAN,  \* unsupportedSyncSource forced
\*     selfInConfig    |-> BOOLEAN,
\*     lastBatchExists |-> BOOLEAN,
\*     batchSafe       |-> BOOLEAN,  \* batch may be safely enqueued
\*     action          |-> Actions ]
VARIABLE history

vars == << history >>

----
\* Helpers / predicates.

InConfigSources == Server

ValidSources == InConfigSources \cup {NotInConfig}

\* Initial-checks short-circuit: the production code returns "do not change"
\* in two cases before consulting topology:
\*   - we are not in the config
\*   - unsupportedSyncSource has been forced via setParameter
NoChangeShortCircuit(d) ==
    \/ ~d.selfInConfig
    \/ d.forceCleared

\* Production code returns the enqueue action when topology requested a change
\* AND the entry point provides metadata (shouldChangeSyncSource, not
\* shouldChangeSyncSourceOnError).
\*
\* The PROPOSED FIX (SERVER-126246): downgrade enqueue to drop when the
\* buffered batch cannot be applied safely on a new sync source.
DecideAction(d) ==
    IF NoChangeShortCircuit(d)
    THEN "kContinueSyncing"
    ELSE IF d.entry = "shouldChangeSyncSource" /\ d.topologyWants
         THEN IF d.lastBatchExists /\ ~d.batchSafe
              THEN "kStopSyncingAndDropLastBatchIfPresent"
              ELSE "kStopSyncingAndEnqueueLastBatch"
         ELSE IF d.entry = "shouldChangeSyncSourceOnError" /\ d.topologyWants
              THEN "kStopSyncingAndDropLastBatchIfPresent"
              ELSE IF d.pingTimeWants
                   THEN "kStopSyncingAndDropLastBatchIfPresent"
                   ELSE "kContinueSyncing"

----
Init == history = << >>

\* ACTION
\* A new shouldChangeSyncSource invocation arrives with non-deterministic
\* inputs drawn from the observable state.  Each invocation appends one
\* decision record to history.  The action is computed by DecideAction.
Decide ==
    /\ Len(history) < MaxDecisions
    /\ \E entry          \in EntryPoints,
          currentSource  \in ValidSources,
          topologyWants  \in BOOLEAN,
          pingTimeWants  \in BOOLEAN,
          forceCleared   \in BOOLEAN,
          selfInConfig   \in BOOLEAN,
          lastBatchExists\in BOOLEAN,
          batchSafe      \in BOOLEAN :
        LET d == [ entry           |-> entry,
                   currentSource   |-> currentSource,
                   topologyWants   |-> topologyWants,
                   pingTimeWants   |-> pingTimeWants,
                   forceCleared    |-> forceCleared,
                   selfInConfig    |-> selfInConfig,
                   lastBatchExists |-> lastBatchExists,
                   batchSafe       |-> batchSafe,
                   action          |-> "kContinueSyncing" ]
            withAction == [ d EXCEPT !.action = DecideAction(d) ]
        IN history' = Append(history, withAction)

Next == Decide

Spec == Init /\ [][Next]_vars /\ WF_vars(Decide)

----
\* Properties (invariants).

\* TypeOK: every recorded decision is well-formed.
TypeOK ==
    \A i \in DOMAIN history :
        /\ history[i].entry  \in EntryPoints
        /\ history[i].action \in Actions
        /\ history[i].currentSource \in ValidSources

\* SAFETY 1 — the edge case targeted by SERVER-126246.
\* If a buffered last batch exists and is NOT safe to enqueue (e.g. its
\* commit timestamp would precede the new sync source's stable timestamp,
\* per BF-43158), the decision must never be kStopSyncingAndEnqueueLastBatch.
NeverEnqueueUnsafeBatch ==
    \A i \in DOMAIN history :
        ~(  history[i].lastBatchExists
         /\ ~history[i].batchSafe
         /\ history[i].action = "kStopSyncingAndEnqueueLastBatch")

\* SAFETY 2 — the error path is documented to be a strict subset of the
\* heartbeat path: shouldChangeSyncSourceOnError must never return the
\* enqueue action.  This codifies the comment in sync_source_selector.h.
ErrorPathNeverEnqueues ==
    \A i \in DOMAIN history :
        ~(  history[i].entry = "shouldChangeSyncSourceOnError"
         /\ history[i].action = "kStopSyncingAndEnqueueLastBatch")

\* SAFETY 3 — short-circuit invariant: when we are not in the config or
\* the unsupportedSyncSource override is set, the action must be
\* kContinueSyncing (no sync-source change is initiated by this node).
ShortCircuitMeansContinue ==
    \A i \in DOMAIN history :
        NoChangeShortCircuit(history[i]) =>
            history[i].action = "kContinueSyncing"

\* SAFETY 4 — pre-fix invariant that we EXPECT to violate, used as a
\* counter-example generator.  If you flip DecideAction back to the
\* pre-fix policy (always return enqueue when topology wants change),
\* TLC will report a violation here.  Kept as documentation; not in cfg.
EnqueueOnlyWhenSafe ==
    \A i \in DOMAIN history :
        history[i].action = "kStopSyncingAndEnqueueLastBatch" =>
            (   ~history[i].lastBatchExists
             \/ history[i].batchSafe)

\* LIVENESS — under fair scheduling we eventually observe MaxDecisions
\* recorded decisions.  Bounds the simulation so liveness checking is
\* well-defined alongside StateConstraint.
EventuallyMakesDecisions ==
    <>(Len(history) = MaxDecisions)

=============================================================================
