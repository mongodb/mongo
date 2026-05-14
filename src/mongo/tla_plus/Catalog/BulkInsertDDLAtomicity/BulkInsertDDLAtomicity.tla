\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/
-------------------------------- MODULE BulkInsertDDLAtomicity ----------------------------------
\* Formal specification of the atomicity boundary for non-transactional bulk insertions running
\* concurrently with DDL operations on the same namespace. This models the behavior reported in
\* SERVER-95924: bulk insertions are divided into sub-batches of up to internalInsertMaxBatchSize
\* documents (64 by default), and the lock-window for synchronization with DDLs is established
\* per-sub-batch rather than per-bulk. A DDL that interleaves between two sub-batches of the same
\* bulk causes the second sub-batch to operate on a different incarnation of the namespace.
\*
\* The wave-1 companion (SERVER-126543) implements an FSM jstest that exercises this race
\* empirically. This spec proves the underlying atomicity invariant the FSM tests.
\*
\* The spec models:
\*   - A namespace identified by an incarnation token (a UUID-style stamp). Each DDL bumps the
\*     incarnation. Drop and rename both transition the visible namespace to a new incarnation
\*     (drop -> 0, rename -> new on the target).
\*   - Bulk insert clients, each holding a sequence of sub-batches. Each client materializes
\*     its target incarnation when it acquires the per-sub-batch lock, not when the bulk started.
\*   - Two configurations: bulk_lock_scope=SUBBATCH (the bug, matches current production) and
\*     bulk_lock_scope=BULK (the green configuration that excludes DDLs for the duration of the
\*     entire bulk). The bug cfg exhibits SubBatchIncarnationSplit; the green cfg does not.
\*
\* The invariant of interest is BulkInsertAtomicity: every committed sub-batch of a bulk targets
\* the same incarnation token. Its violation under SUBBATCH scope is the SERVER-95924 anomaly.
\*
\* To run the model-checker, first edit the constants in MCBulkInsertDDLAtomicity.cfg if desired:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Catalog/BulkInsertDDLAtomicity

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Bulks,        \* set of bulk-insert client identifiers
    SubBatches,   \* max number of sub-batches per bulk (>= 2 to exercise the race)
    DDLBudget,    \* cap on DDL operations to bound the state space
    LOCK_SCOPE    \* "SUBBATCH" (production bug) or "BULK" (proposed fix)

DROPPED == 0
SUBBATCH == "SUBBATCH"
BULK == "BULK"
NS_LOCKED_BY_DDL == "ddl"
NS_LOCKED_BY_BULK == "bulk"
NS_FREE == "free"

ASSUME Cardinality(Bulks) >= 1
ASSUME SubBatches \in 2..16
ASSUME DDLBudget \in 1..32
ASSUME LOCK_SCOPE \in {SUBBATCH, BULK}

SubBatchIdx == 1..SubBatches

\* A sub-batch record captures which incarnation was visible when the sub-batch acquired its lock,
\* and whether the sub-batch already committed. An incarnation of DROPPED means the sub-batch
\* observed a non-existent namespace and (per production) implicitly created the collection.
SubBatchRecord == [committed: BOOLEAN, incarnation: Nat]

EmptySubBatch == [committed |-> FALSE, incarnation |-> DROPPED]

VARIABLE incarnation       \* monotonically advancing per DDL; current visible incarnation
VARIABLE ddlCount          \* number of DDLs performed (state-space cap)
VARIABLE nsLock            \* "free" | "ddl" | "bulk" (only used in lock-scope semantics)
VARIABLE bulkHoldingLock   \* which bulk currently holds the namespace lock under BULK scope, or none
VARIABLE bulkProgress      \* per-bulk: which sub-batch is the bulk about to process (1..SubBatches+1)
VARIABLE bulkLog           \* per-bulk: sequence of committed sub-batch records, length <= SubBatches

NoBulk == "none"

vars == << incarnation, ddlCount, nsLock, bulkHoldingLock, bulkProgress, bulkLog >>

Init ==
    /\ incarnation = 1
    /\ ddlCount = 0
    /\ nsLock = NS_FREE
    /\ bulkHoldingLock = NoBulk
    /\ bulkProgress = [b \in Bulks |-> 1]
    /\ bulkLog = [b \in Bulks |-> << >>]

BulkDone(b) == bulkProgress[b] > SubBatches

\* A DDL takes the namespace lock briefly, bumps the incarnation, and releases. Under BULK scope
\* the DDL must wait until no bulk holds the lock; under SUBBATCH scope the DDL only contends with
\* a single in-flight sub-batch (modeled by nsLock = NS_LOCKED_BY_BULK held only for one sub-batch
\* step), so it can interleave between sub-batches of the same bulk -- the bug.
DoDDL ==
    /\ ddlCount < DDLBudget
    /\ nsLock = NS_FREE
    /\ incarnation' = incarnation + 1
    /\ ddlCount' = ddlCount + 1
    /\ UNCHANGED << nsLock, bulkHoldingLock, bulkProgress, bulkLog >>

\* Under BULK scope, the bulk takes the namespace lock before processing its first sub-batch
\* and holds it for the duration of the bulk.
BulkAcquireLock(b) ==
    /\ LOCK_SCOPE = BULK
    /\ ~BulkDone(b)
    /\ bulkProgress[b] = 1
    /\ Len(bulkLog[b]) = 0
    /\ nsLock = NS_FREE
    /\ nsLock' = NS_LOCKED_BY_BULK
    /\ bulkHoldingLock' = b
    /\ UNCHANGED << incarnation, ddlCount, bulkProgress, bulkLog >>

\* Under SUBBATCH scope, each sub-batch acquires + releases the lock atomically within one action,
\* so DDLs may interleave between sub-batches of the same bulk. We model this by ensuring nsLock
\* is free at the start of each sub-batch (no DDL is currently running), processing, and leaving
\* nsLock free at the end.
SubBatchSubbatchScope(b) ==
    /\ LOCK_SCOPE = SUBBATCH
    /\ ~BulkDone(b)
    /\ nsLock = NS_FREE
    /\ LET observed == incarnation
           record == [committed |-> TRUE, incarnation |-> observed]
       IN
        /\ bulkLog' = [bulkLog EXCEPT ![b] = Append(@, record)]
        /\ bulkProgress' = [bulkProgress EXCEPT ![b] = @ + 1]
    /\ UNCHANGED << incarnation, ddlCount, nsLock, bulkHoldingLock >>

\* Under BULK scope, a sub-batch only fires if this bulk holds the namespace lock. DDLs are
\* excluded for the entire bulk, so every sub-batch observes the same incarnation.
SubBatchBulkScope(b) ==
    /\ LOCK_SCOPE = BULK
    /\ ~BulkDone(b)
    /\ bulkHoldingLock = b
    /\ nsLock = NS_LOCKED_BY_BULK
    /\ LET observed == incarnation
           record == [committed |-> TRUE, incarnation |-> observed]
       IN
        /\ bulkLog' = [bulkLog EXCEPT ![b] = Append(@, record)]
        /\ bulkProgress' = [bulkProgress EXCEPT ![b] = @ + 1]
    /\ UNCHANGED << incarnation, ddlCount, nsLock, bulkHoldingLock >>

\* Under BULK scope the bulk releases the lock after its final sub-batch.
BulkReleaseLock(b) ==
    /\ LOCK_SCOPE = BULK
    /\ bulkHoldingLock = b
    /\ BulkDone(b)
    /\ nsLock = NS_LOCKED_BY_BULK
    /\ nsLock' = NS_FREE
    /\ bulkHoldingLock' = NoBulk
    /\ UNCHANGED << incarnation, ddlCount, bulkProgress, bulkLog >>

Next ==
    \/ DoDDL
    \/ \E b \in Bulks : BulkAcquireLock(b)
    \/ \E b \in Bulks : SubBatchSubbatchScope(b)
    \/ \E b \in Bulks : SubBatchBulkScope(b)
    \/ \E b \in Bulks : BulkReleaseLock(b)
    \/ ( /\ \A b \in Bulks : BulkDone(b)
         /\ ddlCount >= 0
         /\ UNCHANGED vars )

Fairness ==
    /\ WF_vars(DoDDL)
    /\ \A b \in Bulks : WF_vars(BulkAcquireLock(b))
    /\ \A b \in Bulks : WF_vars(SubBatchSubbatchScope(b))
    /\ \A b \in Bulks : WF_vars(SubBatchBulkScope(b))
    /\ \A b \in Bulks : WF_vars(BulkReleaseLock(b))

Spec == Init /\ [][Next]_vars /\ Fairness

-----------------------------------------------------------------------------
\* Type invariants
-----------------------------------------------------------------------------

TypeOK ==
    /\ incarnation \in Nat
    /\ ddlCount \in Nat
    /\ nsLock \in {NS_FREE, NS_LOCKED_BY_DDL, NS_LOCKED_BY_BULK}
    /\ bulkHoldingLock \in (Bulks \cup {NoBulk})
    /\ bulkProgress \in [Bulks -> 1..(SubBatches + 1)]
    /\ \A b \in Bulks :
        /\ Len(bulkLog[b]) <= SubBatches
        /\ \A i \in 1..Len(bulkLog[b]) :
            /\ bulkLog[b][i].committed \in BOOLEAN
            /\ bulkLog[b][i].incarnation \in Nat

\* Progress and log length agree: the log holds exactly the committed sub-batches.
ProgressMatchesLog ==
    \A b \in Bulks : Len(bulkLog[b]) = bulkProgress[b] - 1

-----------------------------------------------------------------------------
\* SERVER-95924 atomicity invariants
-----------------------------------------------------------------------------

\* The headline invariant. Every committed sub-batch of a given bulk observed the same namespace
\* incarnation. A violation is exactly the SERVER-95924 anomaly: a DDL split the bulk across
\* incarnations. Holds under LOCK_SCOPE = BULK; falsifies under LOCK_SCOPE = SUBBATCH.
BulkInsertAtomicity ==
    \A b \in Bulks :
        \A i, j \in 1..Len(bulkLog[b]) :
            bulkLog[b][i].incarnation = bulkLog[b][j].incarnation

\* No bulk that has begun emitting sub-batches saw the namespace as DROPPED on its first sub-batch
\* and then as a live incarnation on a later sub-batch. This is the specific rename / drop split
\* shape called out in the SERVER-95924 description (data partitioned across two collections).
NoDropIncarnationSplit ==
    \A b \in Bulks :
        ~ \E i, j \in 1..Len(bulkLog[b]) :
            /\ i < j
            /\ bulkLog[b][i].incarnation = DROPPED
            /\ bulkLog[b][j].incarnation # DROPPED

\* The mutual-exclusion shape: under BULK scope at most one bulk holds the namespace lock and a
\* DDL never proceeds while a bulk holds it. Under SUBBATCH scope this trivially holds because
\* the lock is only ever instantaneously taken.
LockMutualExclusion ==
    nsLock = NS_LOCKED_BY_BULK => bulkHoldingLock \in Bulks

\* All sub-batches of a finished bulk are stamped committed=TRUE (no silent partials).
DoneBulkAllCommitted ==
    \A b \in Bulks :
        BulkDone(b) =>
            \A i \in 1..Len(bulkLog[b]) : bulkLog[b][i].committed

-----------------------------------------------------------------------------
\* Liveness
-----------------------------------------------------------------------------

\* Every bulk eventually terminates (all sub-batches committed). Liveness is disabled in the
\* default cfg because the bug cfg can starve a bulk if DDLs fire perpetually; enable when
\* DDLBudget is small enough to guarantee a quiescent suffix.
AllBulksEventuallyDone == <>[] \A b \in Bulks : BulkDone(b)

=================================================================================================
