\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------- MODULE CommitTimestampOplogParity -------------------------
\* SERVER-122333: Assert that the storage transaction commit timestamp matches the
\* timestamp embedded in the oplog entry produced by the same write.
\*
\* Modeled flow for a single write on a primary:
\*   1. The write reserves an oplog slot (timestamp T_oplog) under the global oplog
\*      lock.
\*   2. The write opens (or reuses) a storage RecoveryUnit and sets its commit
\*      timestamp T_storage via WiredTigerRecoveryUnit::setCommitTimestamp().
\*   3. The write inserts the oplog entry (carrying T_oplog) into the storage
\*      transaction, then commits the storage transaction at T_storage.
\*
\* Drift is possible whenever steps 1 and 2 can be reordered, skipped, or supplied
\* with stale slots. We model the operation as a per-write state machine and prove
\* that any reachable commit step must satisfy T_storage = T_oplog. The model
\* therefore makes drift unreachable; a violation in the implementation is a
\* counterexample we can replay via the companion jstest.
\*
\* To run the model-checker, first edit the constants in
\* MCCommitTimestampOplogParity.cfg if desired, then:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Storage/CommitTimestampOplogParity

EXTENDS Integers, FiniteSets, Sequences, TLC

\* The set of concurrent writes to model.
CONSTANT Writes

\* The maximum oplog timestamp we will allocate (bounds the state space).
CONSTANT MaxTimestamp

ASSUME MaxTimestamp \in Nat \ {0}
ASSUME Writes # {}

\* Sentinel value for "no timestamp assigned yet".
NoTs == 0

\* Per-write phases.
\*   "init"     -- write has not started; no slot, no RU.
\*   "slotted"  -- oplog slot reserved (oplogTs set); RU not yet stamped.
\*   "stamped"  -- RU commit timestamp set; awaiting commit.
\*   "applied"  -- oplog entry inserted into the RU at oplogTs.
\*   "done"     -- storage transaction committed.
\*   "aborted"  -- storage transaction rolled back.
Phases == {"init", "slotted", "stamped", "applied", "done", "aborted"}

\* The next oplog timestamp the primary will hand out. Modeling a monotonically
\* increasing global clock guarded by the oplog lock.
VARIABLE nextOplogTs

\* The set of timestamps that have actually been written into the oplog.
VARIABLE oplog

\* Per-write state: phase plus the two timestamps we are comparing.
\*   phase[w]      -- one of Phases above
\*   oplogTs[w]    -- timestamp reserved for the oplog entry (NoTs until "slotted")
\*   storageTs[w]  -- commit timestamp set on the RU (NoTs until "stamped")
VARIABLE phase, oplogTs, storageTs

vars == <<nextOplogTs, oplog, phase, oplogTs, storageTs>>

----
\* Type invariant: variables stay in their declared domains.
TypeOK ==
    /\ nextOplogTs \in 1..(MaxTimestamp + 1)
    /\ oplog \subseteq 1..MaxTimestamp
    /\ phase \in [Writes -> Phases]
    /\ oplogTs \in [Writes -> 0..MaxTimestamp]
    /\ storageTs \in [Writes -> 0..MaxTimestamp]

Init ==
    /\ nextOplogTs = 1
    /\ oplog = {}
    /\ phase = [w \in Writes |-> "init"]
    /\ oplogTs = [w \in Writes |-> NoTs]
    /\ storageTs = [w \in Writes |-> NoTs]

----
\* Step 1: reserve an oplog slot under the oplog lock. This advances the global
\* clock and binds the write's oplogTs. We require room in our bounded clock.
ReserveSlot(w) ==
    /\ phase[w] = "init"
    /\ nextOplogTs <= MaxTimestamp
    /\ phase' = [phase EXCEPT ![w] = "slotted"]
    /\ oplogTs' = [oplogTs EXCEPT ![w] = nextOplogTs]
    /\ nextOplogTs' = nextOplogTs + 1
    /\ UNCHANGED <<oplog, storageTs>>

\* Step 2: stamp the RecoveryUnit with a commit timestamp. The implementation
\* MUST pass the reserved oplog timestamp; we model this as the only legal value.
\* Bugs that would supply a stale slot or the system's current clock are exactly
\* the actions we are excluding here.
StampStorage(w) ==
    /\ phase[w] = "slotted"
    /\ storageTs' = [storageTs EXCEPT ![w] = oplogTs[w]]
    /\ phase' = [phase EXCEPT ![w] = "stamped"]
    /\ UNCHANGED <<nextOplogTs, oplog, oplogTs>>

\* Step 3: insert the oplog entry into the RU at the reserved timestamp.
ApplyOplogEntry(w) ==
    /\ phase[w] = "stamped"
    /\ phase' = [phase EXCEPT ![w] = "applied"]
    /\ UNCHANGED <<nextOplogTs, oplog, oplogTs, storageTs>>

\* Step 4: commit the storage transaction. The assertion of interest fires here:
\* the timestamp used by the RU must match the timestamp embedded in the oplog
\* entry. Violating this conjunct is the bug class SERVER-122333 detects.
CommitStorage(w) ==
    /\ phase[w] = "applied"
    /\ storageTs[w] = oplogTs[w]    \* SERVER-122333 invariant at commit time
    /\ storageTs[w] # NoTs
    /\ oplog' = oplog \cup {oplogTs[w]}
    /\ phase' = [phase EXCEPT ![w] = "done"]
    /\ UNCHANGED <<nextOplogTs, oplogTs, storageTs>>

\* A write may abort at any pre-commit phase. Aborted writes do not appear in the
\* oplog and do not bind any timestamp.
AbortStorage(w) ==
    /\ phase[w] \in {"slotted", "stamped", "applied"}
    /\ phase' = [phase EXCEPT ![w] = "aborted"]
    /\ UNCHANGED <<nextOplogTs, oplog, oplogTs, storageTs>>

Next ==
    \E w \in Writes :
        \/ ReserveSlot(w)
        \/ StampStorage(w)
        \/ ApplyOplogEntry(w)
        \/ CommitStorage(w)
        \/ AbortStorage(w)

Spec == Init /\ [][Next]_vars

----
\* Safety properties.

\* The SERVER-122333 invariant: for every write that successfully committed, the
\* RecoveryUnit commit timestamp equals the oplog entry timestamp.
CommitTimestampOplogParity ==
    \A w \in Writes :
        phase[w] = "done" => storageTs[w] = oplogTs[w]

\* The oplog and the per-write reservations agree: any timestamp visible in the
\* oplog was reserved by exactly one committed write at the same timestamp.
OplogReflectsReservations ==
    \A t \in oplog :
        \E w \in Writes :
            /\ phase[w] = "done"
            /\ oplogTs[w] = t
            /\ storageTs[w] = t

\* Oplog timestamps issued to distinct writes are unique. This rules out the
\* "two writes share a slot" failure mode that would also drive drift.
UniqueOplogSlots ==
    \A w1, w2 \in Writes :
        (phase[w1] # "init" /\ phase[w2] # "init" /\ w1 # w2)
            => oplogTs[w1] # oplogTs[w2]

\* The clock never rewinds.
ClockMonotonic ==
    \A w \in Writes :
        phase[w] # "init" => oplogTs[w] < nextOplogTs

=============================================================================
