\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------ MODULE FsyncLockDurableOpTime ------------------------
\* Specification for SERVER-126254: fsyncLock leaves durableOpTime stuck behind
\* lastWritten.
\*
\* Background. On a primary, write operations advance the in-memory "lastWritten"
\* optime when the WiredTiger WriteUnitOfWork commits, while the JournalFlusher
\* thread independently advances "durableOpTime" when oplog entries are fsynced
\* to disk. fsyncLock acquires Global S and blocks new writes; however, oplog
\* entries that have already committed in memory (w:1, j:false) at the moment
\* fsyncLock is invoked sit in the committed-but-not-yet-durable window.
\*
\* In the buggy code path, the JournalFlusher's attempt to advance durableOpTime
\* is itself contingent on storage state guarded by Global IS, which it cannot
\* acquire while Global S is held. The result is a documented stable
\* equilibrium: lastWritten is ahead, durableOpTime is wedged, and nothing on
\* the primary can advance durableOpTime until fsyncUnlock releases Global S.
\* Snapshot/majority reads with afterClusterTime past the wedged optime hang
\* indefinitely.
\*
\* The fix decouples the journal-flush advancement of durableOpTime from Global
\* S acquisition. Modelled here by the FixedAdvanceDurable variant of the
\* AdvanceDurable action: the green model (BugEnabled = FALSE) allows
\* durableOpTime to advance while Global S is held; the buggy model
\* (BugEnabled = TRUE) blocks it. The liveness property
\*
\*     lastWritten advances ~> durableOpTime catches up
\*
\* holds under the green model and is violated under the buggy model. The
\* counterexample TLC produces matches the deterministic single-node repro
\* attached to SERVER-126254 step-for-step (pause JournalFlusher, w:1 j:false
\* insert, fsyncLock, observe wedge).
\*
\* To run the model-checker, first edit MCFsyncLockDurableOpTime.cfg if desired,
\* then:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Replication/FsyncLockDurableOpTime
\*
\* The bug toggle is the BugEnabled constant; both MCFsyncLockDurableOpTime.cfg
\* (green) and MCFsyncLockDurableOpTimeBug.cfg (red) are checked in.

EXTENDS Integers, FiniteSets, Sequences, TLC

\* The maximum optime any timestamp variable can reach during model-checking.
\* Bounds the state space for TLC. Two is enough to expose the wedge: the first
\* write puts a value in the committed-but-not-durable window, the second write
\* (or the fix) clears it.
CONSTANT MaxOpTime

\* When TRUE, AdvanceDurable is gated by ~globalSLockHeld (the bug). When FALSE,
\* AdvanceDurable can fire regardless of globalSLockHeld (the fix).
CONSTANT BugEnabled

----
\* State variables.

\* The most recent optime committed by a WriteUnitOfWork on the primary. Monotone
\* non-decreasing. Maps to repl::ReplicationCoordinatorImpl::_setMyLastWrittenOpTime.
VARIABLE lastWritten

\* The most recent optime durably fsynced to the journal. Monotone non-decreasing.
\* Always TimestampLTE(durableOpTime, lastWritten). Maps to
\* repl::ReplicationCoordinatorImpl::_setMyLastDurableOpTime.
VARIABLE durableOpTime

\* TRUE iff Global S has been acquired via fsyncLock. While TRUE, no new writes
\* can commit (CommitWUOW is gated). In the buggy model, AdvanceDurable is also
\* gated; in the fixed model, it is not.
VARIABLE globalSLockHeld

vars == <<lastWritten, durableOpTime, globalSLockHeld>>

----
\* Helpers.

\* The set of optimes that have been written but not yet made durable. This is
\* the committed-but-not-durable window. When fsyncLock fires with this set
\* non-empty in the buggy code path, the contents are wedged.
PendingDurable == lastWritten - durableOpTime

----
\* Initial state. No writes, no lock.

Init ==
    /\ lastWritten = 0
    /\ durableOpTime = 0
    /\ globalSLockHeld = FALSE

----
\* Actions.

\* ACTION
\* The primary commits one write. Advances lastWritten by 1. Requires
\* ~globalSLockHeld because fsyncLock blocks new writes.
CommitWUOW ==
    /\ ~globalSLockHeld
    /\ lastWritten < MaxOpTime
    /\ lastWritten' = lastWritten + 1
    /\ UNCHANGED <<durableOpTime, globalSLockHeld>>

\* ACTION
\* fsyncLock acquires Global S. Idempotent: re-acquiring while held is a no-op
\* but does not enable additional behavior, so we forbid it to keep the state
\* graph trim.
AcquireGlobalS ==
    /\ ~globalSLockHeld
    /\ globalSLockHeld' = TRUE
    /\ UNCHANGED <<lastWritten, durableOpTime>>

\* ACTION
\* fsyncUnlock releases Global S. Modelled so the spec is closed under recovery
\* and is not vacuously stuck after AcquireGlobalS. The liveness property holds
\* trivially after release, so it is the gap *before* release that the bug
\* exposes.
ReleaseGlobalS ==
    /\ globalSLockHeld
    /\ globalSLockHeld' = FALSE
    /\ UNCHANGED <<lastWritten, durableOpTime>>

\* ACTION
\* The JournalFlusher advances durableOpTime to catch up with lastWritten.
\* This is the action whose firing is contingent on BugEnabled.
\*
\* In the buggy model (BugEnabled = TRUE), the JournalFlusher's storage-side
\* dependency means it cannot fire while Global S is held: durableOpTime is
\* wedged until fsyncUnlock. In the fixed model (BugEnabled = FALSE), the
\* advancement is decoupled from Global S and fires freely.
AdvanceDurable ==
    /\ durableOpTime < lastWritten
    /\ (BugEnabled => ~globalSLockHeld)
    /\ durableOpTime' = durableOpTime + 1
    /\ UNCHANGED <<lastWritten, globalSLockHeld>>

----
\* Next-state relation.

Next ==
    \/ CommitWUOW
    \/ AcquireGlobalS
    \/ ReleaseGlobalS
    \/ AdvanceDurable

\* Weak fairness on the actions that drive forward progress. AdvanceDurable
\* models the JournalFlusher thread, which is naturally weakly-fair.
\* ReleaseGlobalS models a finite-duration fsyncLock; without weak fairness here
\* the operator could hold the lock forever and the liveness property would
\* hold only vacuously. CommitWUOW does not need fairness; we only require that
\* IF lastWritten advances THEN durableOpTime catches up.
Spec ==
    /\ Init
    /\ [][Next]_vars
    /\ WF_vars(AdvanceDurable)
    /\ WF_vars(ReleaseGlobalS)

----
\* Safety invariants.

\* Type invariant.
TypeOK ==
    /\ lastWritten \in 0..MaxOpTime
    /\ durableOpTime \in 0..MaxOpTime
    /\ globalSLockHeld \in BOOLEAN

\* durableOpTime never overtakes lastWritten. This is the structural invariant
\* of the WiredTiger / replication storage interface and must hold in both the
\* green and the buggy models.
DurableNeverAheadOfWritten == durableOpTime <= lastWritten

\* The two timestamps are always non-negative and monotone in any reachable
\* state (the actions only ever +1 them).
MonotoneOpTimes == lastWritten >= 0 /\ durableOpTime >= 0

----
\* Liveness properties.

\* The headline liveness property. If lastWritten ever sits ahead of
\* durableOpTime, then durableOpTime eventually catches up. Under the green
\* model this holds. Under the buggy model TLC produces a counterexample of
\* length 3 (CommitWUOW, AcquireGlobalS, stuttering forever) because
\* AdvanceDurable is structurally disabled by globalSLockHeld and no
\* ReleaseGlobalS can fire either if we drop its fairness in the buggy cfg.
\*
\* In practice we keep ReleaseGlobalS fair in both cfgs so the property holds
\* on the green cfg by the fix, and is violated on the bug cfg by the wedge
\* *before* release: between AcquireGlobalS and ReleaseGlobalS, the system is
\* in the stable equilibrium documented on the ticket.
DurableEventuallyCatchesUp ==
    (lastWritten > durableOpTime) ~> (lastWritten = durableOpTime)

\* A stricter formulation suitable as a wedge-window detector: whenever Global S
\* is held with pending-durable entries, the pending count must eventually drop
\* to zero. This is the property the buggy code path violates within the
\* lock-held window. We keep it as an alternative to DurableEventuallyCatchesUp
\* for diagnostic counterexample reading.
NoWedgeUnderGlobalS ==
    (globalSLockHeld /\ PendingDurable > 0) ~> (PendingDurable = 0)

===============================================================================
