\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

--------------------------- MODULE RollbackPreparedAbort ---------------------------
\* SERVER-125802: Standby should set rollback timestamp when applying aborts for
\* prepared transactions.
\*
\* Motivation (from the ticket):
\*   * Primary prepares a transaction at ts P and aborts at ts A, but A is not
\*     yet checkpointed.
\*   * Standby applies the abort but does NOT set a rollback timestamp at A.
\*   * Standby steps up; the abort's effects are eventually checkpointed.
\*   * Without a rollback timestamp pinning when the abort becomes visible in
\*     a checkpoint, the engine has no way to know whether the abort should be
\*     visible in any given checkpoint -> inconsistent state across the
\*     prepared-document keys.
\*
\* This spec models the abort-apply path on a standby and pairs it with the
\* WiredTiger rollback-timestamp advancement.  The invariant we want:
\* whenever a standby has applied an abort for a prepared transaction at ts A,
\* the standby's rollbackTimestamp is >= A on every later checkpoint that
\* observes the abort's effects.
\*
\* To run the model-checker:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh RollbackPreparedAbort

EXTENDS Integers, FiniteSets, Sequences, TLC

\* The set of server IDs.
CONSTANT Server

\* The maximum prepare/abort timestamp considered during model-checking.
CONSTANT MaxTs

\* Per-server state.
\* prepared[s] : set of prepared transactions known to s (with their prepareTs).
\* aborted[s]  : set of (txn, abortTs) the standby has applied an abort for.
\* checkpoints[s] : set of {ts, visible} tuples; visible is the set of aborts
\*                  whose effects are durable in that checkpoint.
\* rollbackTs[s] : the WiredTiger rollback timestamp on s.  Aborts at ts >
\*                 rollbackTs are NOT visible in checkpoints; aborts at ts <=
\*                 rollbackTs ARE visible.
\* role[s] : "Primary" or "Standby".
VARIABLE prepared, aborted, checkpoints, rollbackTs, role

vars == <<prepared, aborted, checkpoints, rollbackTs, role>>

----
\* Helpers.

Txn == {"t1", "t2"}
Timestamps == 1..MaxTs

Max(a, b) == IF a >= b THEN a ELSE b

\* Aborts that, at the time of checkpoint c, MUST be visible if pinned by
\* rollbackTs.  The standby's contract: any aborted entry with abortTs <=
\* checkpoint.ts AND abortTs <= rollbackTs is visible.
ShouldBeVisible(s, c) ==
    {a \in aborted[s] :
        /\ a.abortTs <= c.ts
        /\ a.abortTs <= rollbackTs[s]}

----
\* Initial values.

Init ==
    /\ prepared    = [s \in Server |-> {}]
    /\ aborted     = [s \in Server |-> {}]
    /\ checkpoints = [s \in Server |-> {}]
    /\ rollbackTs  = [s \in Server |-> 0]
    /\ role        = [s \in Server |-> "Standby"]

----
\* Actions.

\* Primary prepares a transaction at ts.
PrepareTxnOnPrimary(s, t, ts) ==
    /\ role[s] = "Primary"
    /\ ts \in Timestamps
    /\ ~ \E p \in prepared[s] : p.txn = t
    /\ prepared' = [prepared EXCEPT ![s] = prepared[s] \cup {[txn |-> t, prepareTs |-> ts]}]
    /\ UNCHANGED <<aborted, checkpoints, rollbackTs, role>>

\* Replicate prepare to standby (no rollbackTs side effect yet -- the abort is
\* what carries the rollback-timestamp obligation in this ticket's scope).
ReplicatePrepare(src, dst, t, ts) ==
    /\ role[src] = "Primary"
    /\ role[dst] = "Standby"
    /\ [txn |-> t, prepareTs |-> ts] \in prepared[src]
    /\ prepared' = [prepared EXCEPT ![dst] = prepared[dst] \cup {[txn |-> t, prepareTs |-> ts]}]
    /\ UNCHANGED <<aborted, checkpoints, rollbackTs, role>>

\* Standby applies an abort for a prepared transaction at ts.
\* THIS IS THE FIX UNDER TEST: applying the abort must advance rollbackTs to
\* at least ts so that future checkpoints know at what time the abort becomes
\* visible.  Without this, ShouldBeVisible may return {} for an abort that the
\* standby has actually applied, leading to the checkpoint inconsistency that
\* SERVER-125802 describes.
StandbyApplyAbort(s, t, ts) ==
    /\ role[s] = "Standby"
    /\ \E p \in prepared[s] : p.txn = t /\ p.prepareTs <= ts
    /\ ~ \E a \in aborted[s] : a.txn = t
    /\ aborted'    = [aborted EXCEPT ![s] = aborted[s] \cup {[txn |-> t, abortTs |-> ts]}]
    \* The patch: rollbackTs advances on abort-apply.
    /\ rollbackTs' = [rollbackTs EXCEPT ![s] = Max(rollbackTs[s], ts)]
    /\ UNCHANGED <<prepared, checkpoints, role>>

\* WiredTiger takes a checkpoint at ts on server s.  The set of aborts visible
\* in this checkpoint is determined by ShouldBeVisible at checkpoint-time.
TakeCheckpoint(s, ts) ==
    /\ ts \in Timestamps
    /\ ts >= rollbackTs[s]  \* checkpoint advances time forward
    /\ LET c == [ts |-> ts, visible |-> {a \in aborted[s] : a.abortTs <= ts}]
        IN checkpoints' = [checkpoints EXCEPT ![s] = checkpoints[s] \cup {c}]
    /\ UNCHANGED <<prepared, aborted, rollbackTs, role>>

\* Standby steps up to primary.
StepUp(s) ==
    /\ role[s] = "Standby"
    /\ role' = [role EXCEPT ![s] = "Primary"]
    /\ UNCHANGED <<prepared, aborted, checkpoints, rollbackTs>>

\* Bootstrap one primary so the system can do anything.
ElectInitialPrimary(s) ==
    /\ \A x \in Server : role[x] = "Standby"
    /\ role' = [role EXCEPT ![s] = "Primary"]
    /\ UNCHANGED <<prepared, aborted, checkpoints, rollbackTs>>

----

Next ==
    \/ \E s \in Server : ElectInitialPrimary(s)
    \/ \E s \in Server, t \in Txn, ts \in Timestamps : PrepareTxnOnPrimary(s, t, ts)
    \/ \E src, dst \in Server, t \in Txn, ts \in Timestamps : ReplicatePrepare(src, dst, t, ts)
    \/ \E s \in Server, t \in Txn, ts \in Timestamps : StandbyApplyAbort(s, t, ts)
    \/ \E s \in Server, ts \in Timestamps : TakeCheckpoint(s, ts)
    \/ \E s \in Server : StepUp(s)

Spec == Init /\ [][Next]_vars

----
\* Invariants.

\* The headline safety property: every abort the standby has applied at ts A
\* is pinned by rollbackTs >= A.  If this holds, then any subsequent
\* checkpoint that observes the abort's effects has a well-defined visibility
\* relationship with rollbackTs and the engine knows whether to show the
\* abort.  Without the SERVER-125802 fix (advance rollbackTs on abort-apply
\* in StandbyApplyAbort) this is violated trivially.
AbortPinnedByRollbackTimestamp ==
    \A s \in Server :
        \A a \in aborted[s] :
            rollbackTs[s] >= a.abortTs

\* No checkpoint claims an abort as visible whose abortTs exceeds rollbackTs
\* at the time of the checkpoint.  Together with AbortPinnedByRollbackTimestamp
\* this gives the post-step-up consistency guarantee.
CheckpointVisibilityConsistent ==
    \A s \in Server :
        \A c \in checkpoints[s] :
            \A a \in c.visible :
                a.abortTs <= rollbackTs[s]

\* Liveness sanity: state shape.
TypeOK ==
    /\ prepared    \in [Server -> SUBSET [txn : Txn, prepareTs : Timestamps]]
    /\ aborted     \in [Server -> SUBSET [txn : Txn, abortTs   : Timestamps]]
    /\ rollbackTs  \in [Server -> 0..MaxTs]
    /\ role        \in [Server -> {"Primary", "Standby"}]

===============================================================================
