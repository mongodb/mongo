\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------- MODULE OplogVisibilityThread -------------------------
\*
\* A specification of the oplog visibility thread lifecycle as it is driven by
\* the side effect of getRecordStore() during durable recovery of the oplog.
\*
\* Background (SERVER-122142):
\*   The WiredTigerOplogManager owns a single visibility thread. The thread is
\*   started by WiredTigerRecordStore::Oplog::Oplog(...) and stopped by the
\*   destructor. When DDL on the oplog forces a durable recovery from disk, any
\*   reader that calls getRecordStore() can cause a new Oplog instance to be
\*   constructed: that construction stops any prior thread and starts a fresh
\*   one. Multiple concurrent readers can therefore drive start/stop overlap on
\*   the same OplogManager without the catalog enforcing single-writer
\*   ordering. The intended behavior is that exactly one start/stop is in
\*   flight at any moment and no reader sees the thread torn down while it
\*   still holds a pin on the underlying Oplog record store.
\*
\* Variables track:
\*   - threadState:   "stopped" | "starting" | "running" | "stopping"
\*   - inflight:      count of start/stop operations currently mutating state
\*   - pinned:        set of readers that have pinned the current Oplog
\*                    instance (i.e. they hold a cursor / are mid-read)
\*   - readerPC:      per-reader program counter ("idle", "acquired",
\*                    "recovering", "starting", "stopping", "pinned", "done")
\*   - oplogEpoch:    monotonically increasing tag for the current Oplog
\*                    instance; bumped on each (re)construction
\*   - readerEpoch:   the epoch each reader observed when it pinned
\*   - opsCount:      bounded counter so model-checking terminates
\*
\* Bug toggle:
\*   AllowConcurrentStartStop = TRUE  models the buggy state today: readers
\*   can race through getRecordStore() and overlap each other's start/stop.
\*   AllowConcurrentStartStop = FALSE models the fix where the catalog
\*   serializes the side-effecting transition with a coarse lock.
\*
\* Invariants of interest (see below):
\*   AtMostOneStartStopInFlight
\*   NoThreadTeardownWhileReaderPinned
\*   PinnedReaderSeesRunningThread
\*   EpochMonotonic
\*

EXTENDS Integers, FiniteSets, Sequences, TLC

CONSTANTS
    \* Set of reader operation IDs.
    Readers,
    \* Maximum number of recovery-triggering operations before model-checking
    \* halts. Bounds the state space.
    MaxOps,
    \* If TRUE, the spec permits two readers to enter the start/stop critical
    \* section simultaneously (the SERVER-122142 bug). If FALSE, the spec
    \* requires the transition to be serialized.
    AllowConcurrentStartStop

(***************************************************************************)
(* Variables.                                                              *)
(***************************************************************************)

\* The visibility thread's lifecycle state.
VARIABLE threadState

\* Number of readers currently inside the start/stop critical section.
VARIABLE inflight

\* The set of readers that have pinned the current Oplog instance.
VARIABLE pinned

\* Per-reader program counter.
VARIABLE readerPC

\* Tag identifying the current Oplog instance. Incremented on every
\* (re)construction.
VARIABLE oplogEpoch

\* Each reader's observed epoch at the moment it pinned the oplog.
VARIABLE readerEpoch

\* Bounded global op counter.
VARIABLE opsCount

vars == <<threadState, inflight, pinned, readerPC, oplogEpoch,
          readerEpoch, opsCount>>

(***************************************************************************)
(* Helpers.                                                                *)
(***************************************************************************)

ThreadStates == {"stopped", "starting", "running", "stopping"}

ReaderPCStates ==
    {"idle", "acquired", "recovering", "starting", "stopping",
     "pinned", "done"}

NoReader == CHOOSE r : r \notin Readers

(***************************************************************************)
(* Initial state.                                                          *)
(***************************************************************************)

Init ==
    /\ threadState = "stopped"
    /\ inflight = 0
    /\ pinned = {}
    /\ readerPC = [r \in Readers |-> "idle"]
    /\ oplogEpoch = 0
    /\ readerEpoch = [r \in Readers |-> 0]
    /\ opsCount = 0

(***************************************************************************)
(* Actions.                                                                *)
(***************************************************************************)

\* A reader r enters getRecordStore() and acquires the catalog handle. This
\* models a query that has just resolved the oplog namespace.
AcquireHandle(r) ==
    /\ readerPC[r] = "idle"
    /\ opsCount < MaxOps
    /\ readerPC' = [readerPC EXCEPT ![r] = "acquired"]
    /\ opsCount' = opsCount + 1
    /\ UNCHANGED <<threadState, inflight, pinned, oplogEpoch, readerEpoch>>

\* The reader observes that the oplog requires durable recovery (DDL has
\* occurred and the cached RecordStore is gone). The reader is now committed
\* to (re)constructing the Oplog instance, which side-effects start/stop.
NeedRecovery(r) ==
    /\ readerPC[r] = "acquired"
    /\ readerPC' = [readerPC EXCEPT ![r] = "recovering"]
    /\ UNCHANGED <<threadState, inflight, pinned, oplogEpoch, readerEpoch,
                   opsCount>>

\* The reader has a handle and the cached Oplog is alive: pin it without
\* triggering recovery. This is the common-case path that should be
\* unaffected by the bug.
PinExistingOplog(r) ==
    /\ readerPC[r] = "acquired"
    /\ threadState = "running"
    /\ readerPC' = [readerPC EXCEPT ![r] = "pinned"]
    /\ pinned' = pinned \cup {r}
    /\ readerEpoch' = [readerEpoch EXCEPT ![r] = oplogEpoch]
    /\ UNCHANGED <<threadState, inflight, oplogEpoch, opsCount>>

\* Begin tearing the old visibility thread down. This is the
\* WiredTigerRecordStore::Oplog::~Oplog() side of recovery: it calls
\* OplogManager::stop().
\*
\* The bug allows this transition even while another reader is also
\* mid-transition (inflight > 0) or still pinned. The fix forces both
\* inflight = 0 and pinned = {}, modeling a coarse catalog lock that
\* serializes the side effect and waits for in-flight readers to drain.
BeginStop(r) ==
    /\ readerPC[r] = "recovering"
    /\ threadState \in {"running", "starting"}
    /\ \/ AllowConcurrentStartStop
       \/ /\ inflight = 0
          /\ pinned = {}
    /\ threadState' = "stopping"
    /\ inflight' = inflight + 1
    /\ readerPC' = [readerPC EXCEPT ![r] = "stopping"]
    /\ UNCHANGED <<pinned, oplogEpoch, readerEpoch, opsCount>>

\* Finish tearing the visibility thread down. The bug allows this even when
\* pinned # {} (i.e. a reader is still using the old oplog); the fix forbids
\* it.
FinishStop(r) ==
    /\ readerPC[r] = "stopping"
    /\ threadState = "stopping"
    /\ \/ AllowConcurrentStartStop
       \/ pinned = {}
    /\ threadState' = "stopped"
    /\ inflight' = inflight - 1
    /\ readerPC' = [readerPC EXCEPT ![r] = "recovering"]
    /\ UNCHANGED <<pinned, oplogEpoch, readerEpoch, opsCount>>

\* Begin starting a new visibility thread. This is the
\* WiredTigerRecordStore::Oplog::Oplog() side of recovery: it calls
\* OplogManager::start() against the newly-constructed RecordStore.
BeginStart(r) ==
    /\ readerPC[r] = "recovering"
    /\ threadState = "stopped"
    /\ \/ AllowConcurrentStartStop
       \/ inflight = 0
    /\ threadState' = "starting"
    /\ inflight' = inflight + 1
    /\ oplogEpoch' = oplogEpoch + 1
    /\ readerPC' = [readerPC EXCEPT ![r] = "starting"]
    /\ UNCHANGED <<pinned, readerEpoch, opsCount>>

\* Finish starting the visibility thread. The reader that drove the recovery
\* pins the freshly-constructed Oplog (it owns the construction frame).
FinishStart(r) ==
    /\ readerPC[r] = "starting"
    /\ threadState = "starting"
    /\ threadState' = "running"
    /\ inflight' = inflight - 1
    /\ readerPC' = [readerPC EXCEPT ![r] = "pinned"]
    /\ pinned' = pinned \cup {r}
    /\ readerEpoch' = [readerEpoch EXCEPT ![r] = oplogEpoch]
    /\ UNCHANGED <<oplogEpoch, opsCount>>

\* Reader unpins (its cursor closes / it completes its read).
Unpin(r) ==
    /\ readerPC[r] = "pinned"
    /\ readerPC' = [readerPC EXCEPT ![r] = "done"]
    /\ pinned' = pinned \ {r}
    /\ UNCHANGED <<threadState, inflight, oplogEpoch, readerEpoch, opsCount>>

\* Reader resets and may be re-issued.
Reset(r) ==
    /\ readerPC[r] = "done"
    /\ readerPC' = [readerPC EXCEPT ![r] = "idle"]
    /\ UNCHANGED <<threadState, inflight, pinned, oplogEpoch, readerEpoch,
                   opsCount>>

\* Quiescent stutter: once the op budget is exhausted and every reader has
\* settled back to "idle", stutter forever rather than report a deadlock.
\* This is a model-checking convenience; nothing in the system under test
\* changes.
Quiesce ==
    /\ opsCount >= MaxOps
    /\ \A r \in Readers : readerPC[r] \in {"idle", "done"}
    /\ UNCHANGED vars

Next ==
    \/ \E r \in Readers :
        \/ AcquireHandle(r)
        \/ NeedRecovery(r)
        \/ PinExistingOplog(r)
        \/ BeginStop(r)
        \/ FinishStop(r)
        \/ BeginStart(r)
        \/ FinishStart(r)
        \/ Unpin(r)
        \/ Reset(r)
    \/ Quiesce

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Invariants.                                                             *)
(***************************************************************************)

\* AtMostOneStartStopInFlight: only one reader at a time may be mutating the
\* OplogManager's lifecycle. This invariant is VIOLATED when
\* AllowConcurrentStartStop = TRUE.
AtMostOneStartStopInFlight == inflight <= 1

\* NoThreadTeardownWhileReaderPinned: a reader holding a pin on the current
\* Oplog must never observe the visibility thread torn down beneath it.
\* Captured as: if any reader is pinned at the latest epoch, threadState is
\* not "stopped" or "stopping".
NoThreadTeardownWhileReaderPinned ==
    \A r \in pinned :
        readerEpoch[r] = oplogEpoch =>
            threadState \in {"running", "starting"}

\* PinnedReaderSeesRunningThread: a reader that pinned at the current epoch
\* either sees "running" or "starting" (a started-and-not-yet-stopped thread).
PinnedReaderSeesRunningThread ==
    \A r \in pinned :
        readerEpoch[r] = oplogEpoch =>
            threadState # "stopped"

\* EpochMonotonic: epoch never decreases.
EpochMonotonic == oplogEpoch >= 0

\* TypeOK: variable types. The epoch can grow up to one per BeginStart, and
\* BeginStart is bounded by NeedRecovery -> AcquireHandle, which is bounded
\* by MaxOps; we leave generous headroom.
TypeOK ==
    /\ threadState \in ThreadStates
    /\ inflight \in 0..Cardinality(Readers)
    /\ pinned \subseteq Readers
    /\ readerPC \in [Readers -> ReaderPCStates]
    /\ oplogEpoch \in 0..(MaxOps * 2)
    /\ readerEpoch \in [Readers -> 0..(MaxOps * 2)]
    /\ opsCount \in 0..MaxOps

================================================================================
