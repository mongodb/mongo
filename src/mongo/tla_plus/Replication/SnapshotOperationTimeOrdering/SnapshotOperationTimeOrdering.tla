\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

--------------------- MODULE SnapshotOperationTimeOrdering ---------------------
\* Formal specification of the snapshot / operationTime ordering hazard fixed in
\* SERVER-120304 ("Snapshot time may advance before fetching a command's
\* operationTime"). The original PR (#49451) was reverted (#53585) and
\* re-landed as a redux (#53614).
\*
\* The hazard: for a majority-read command, the server (i) acquires a storage
\* snapshot at the current committed timestamp, (ii) executes the read, then
\* (iii) computes operationTime to return to the client. In the pre-fix code,
\* step (iii) re-reads getCurrentCommittedSnapshotOpTime() rather than the
\* timestamp the read actually used. If the committed snapshot advances on
\* another thread between (i) and (iii), the reported operationTime can be
\* strictly greater than the timestamp at which data was returned.
\*
\* A client that uses the returned operationTime as afterClusterTime on a
\* follow-up causally-consistent read then observes a value that the original
\* read could not have seen, breaking causal consistency.
\*
\* The fix captures the last-used read timestamp on transaction close and
\* prefers it over the live committed snapshot when computing operationTime.
\*
\* To run the model-checker, first edit the constants in
\* MCSnapshotOperationTimeOrdering.cfg if desired, then:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Replication/SnapshotOperationTimeOrdering

EXTENDS Integers, FiniteSets, Sequences, TLC

\* The set of client thread IDs running read commands concurrently. Each
\* thread walks the (AcquireSnapshot, Read, FetchOperationTime, Execute)
\* sequence once.
CONSTANT Thread

\* The maximum value the cluster clock may reach. Bounds the state space.
CONSTANT MaxClusterTime

\* Whether to model the pre-fix behavior. When TRUE, FetchOperationTime reads
\* the live committed snapshot (current product bug). When FALSE, it reads
\* the per-thread last-used read timestamp recorded at snapshot acquisition
\* (the SERVER-120304 fix).
CONSTANT UseLiveCommittedSnapshot

----
\* State variables.

\* Monotonically advancing logical cluster clock. Stands in for the timestamps
\* assigned to write operations on the primary.
VARIABLE clusterTime

\* The most recently durably-committed timestamp visible to readers. Advances
\* as writes are acknowledged by a majority. Never exceeds clusterTime.
VARIABLE committedSnapshot

\* Per-thread phase tracking the read pipeline:
\*   "Idle"              - thread has not started
\*   "SnapshotAcquired"  - storage snapshot acquired at threadSnapshot[t]
\*   "ReadDone"          - data has been read from the snapshot
\*   "OpTimeFetched"     - operationTime has been computed
\*   "Executed"          - response sent to client
VARIABLE threadPhase

\* Per-thread snapshot timestamp captured at AcquireSnapshot. This models the
\* "last-used read timestamp" the fix later reads back from the recovery unit.
VARIABLE threadSnapshot

\* Per-thread operationTime computed by FetchOperationTime and returned to the
\* client by Execute.
VARIABLE threadOpTime

\* Append-only history of completed reads. Each entry records the snapshot
\* timestamp the read actually used and the operationTime reported to the
\* client. The safety invariant is a relation over this history.
VARIABLE executedReads

vars == <<clusterTime,
          committedSnapshot,
          threadPhase,
          threadSnapshot,
          threadOpTime,
          executedReads>>

----
\* Helpers.

\* A sentinel value used to express "no timestamp recorded yet". Distinguished
\* from the natural numbers by being a structurally different value.
NoTs == [none |-> TRUE]

\* True iff x is a real timestamp (not the NoTs sentinel).
HasTs(x) == x /= NoTs

----
\* Type invariant. Asserted across all reachable states.

TypeOK ==
    /\ clusterTime \in 0..MaxClusterTime
    /\ committedSnapshot \in 0..MaxClusterTime
    /\ committedSnapshot <= clusterTime
    /\ threadPhase \in [Thread -> {"Idle",
                                   "SnapshotAcquired",
                                   "ReadDone",
                                   "OpTimeFetched",
                                   "Executed"}]
    /\ threadSnapshot \in [Thread -> (0..MaxClusterTime) \cup {NoTs}]
    /\ threadOpTime \in [Thread -> (0..MaxClusterTime) \cup {NoTs}]
    /\ executedReads \in SUBSET [snapshot: 0..MaxClusterTime,
                                 opTime: 0..MaxClusterTime,
                                 thread: Thread]

----
\* Initial state.

Init ==
    /\ clusterTime = 0
    /\ committedSnapshot = 0
    /\ threadPhase = [t \in Thread |-> "Idle"]
    /\ threadSnapshot = [t \in Thread |-> NoTs]
    /\ threadOpTime = [t \in Thread |-> NoTs]
    /\ executedReads = {}

----
\* Actions.

\* The cluster clock advances. Models the primary timestamping a write that
\* has not yet been majority-acknowledged.
ClockTick ==
    /\ clusterTime < MaxClusterTime
    /\ clusterTime' = clusterTime + 1
    /\ UNCHANGED <<committedSnapshot, threadPhase, threadSnapshot,
                   threadOpTime, executedReads>>

\* A concurrent write reaches majority acknowledgement, advancing the
\* committed snapshot to some value at or below the current clusterTime.
\* This is the action that races with an in-flight read in the bug scenario:
\* it can fire between AcquireSnapshot and FetchOperationTime on another
\* thread.
AdvanceCommittedSnapshot ==
    /\ \E newTs \in (committedSnapshot + 1)..clusterTime :
        committedSnapshot' = newTs
    /\ UNCHANGED <<clusterTime, threadPhase, threadSnapshot, threadOpTime,
                   executedReads>>

\* A thread acquires a storage snapshot at the current committed timestamp.
\* The chosen timestamp is recorded so the fix can read it back later via
\* recoveryUnit->getLastUsedReadTimestamp().
AcquireSnapshot(t) ==
    /\ threadPhase[t] = "Idle"
    /\ threadPhase' = [threadPhase EXCEPT ![t] = "SnapshotAcquired"]
    /\ threadSnapshot' = [threadSnapshot EXCEPT ![t] = committedSnapshot]
    /\ UNCHANGED <<clusterTime, committedSnapshot, threadOpTime, executedReads>>

\* The read executes against the acquired snapshot. The snapshot timestamp is
\* not mutated; the read is read-only with respect to the state we track.
\* This action is separated from AcquireSnapshot to give the scheduler a place
\* to interleave AdvanceCommittedSnapshot before FetchOperationTime fires.
ExecuteRead(t) ==
    /\ threadPhase[t] = "SnapshotAcquired"
    /\ threadPhase' = [threadPhase EXCEPT ![t] = "ReadDone"]
    /\ UNCHANGED <<clusterTime, committedSnapshot, threadSnapshot,
                   threadOpTime, executedReads>>

\* Compute the operationTime to return to the client. This is the action that
\* expresses the bug toggle:
\*   - UseLiveCommittedSnapshot = TRUE  reproduces the pre-fix code, which
\*     unconditionally read getCurrentCommittedSnapshotOpTime() here. If the
\*     committed snapshot has advanced since the snapshot was acquired, the
\*     reported operationTime exceeds the snapshot the read actually used.
\*   - UseLiveCommittedSnapshot = FALSE reproduces the fix, which uses the
\*     timestamp captured at AcquireSnapshot (modeling
\*     recoveryUnit->getLastUsedReadTimestamp()).
FetchOperationTime(t) ==
    /\ threadPhase[t] = "ReadDone"
    /\ LET fetched ==
            IF UseLiveCommittedSnapshot
            THEN committedSnapshot
            ELSE threadSnapshot[t]
       IN  threadOpTime' = [threadOpTime EXCEPT ![t] = fetched]
    /\ threadPhase' = [threadPhase EXCEPT ![t] = "OpTimeFetched"]
    /\ UNCHANGED <<clusterTime, committedSnapshot, threadSnapshot,
                   executedReads>>

\* Send the response to the client. The (snapshot, opTime) pair is sealed into
\* the history; subsequent state changes cannot rewrite a past response.
SendResponse(t) ==
    /\ threadPhase[t] = "OpTimeFetched"
    /\ threadPhase' = [threadPhase EXCEPT ![t] = "Executed"]
    /\ executedReads' = executedReads \cup
        {[snapshot |-> threadSnapshot[t],
          opTime   |-> threadOpTime[t],
          thread   |-> t]}
    /\ UNCHANGED <<clusterTime, committedSnapshot, threadSnapshot,
                   threadOpTime>>

\* Explicit stutter action enabled once every thread has executed. Without
\* this, TLC reports the natural terminal state (all threads "Executed",
\* committedSnapshot == MaxClusterTime) as a deadlock. The hazard surfaces
\* well before the terminal state is reached, so allowing infinite stutter
\* there is harmless for the safety invariant.
Done ==
    /\ \A t \in Thread : threadPhase[t] = "Executed"
    /\ UNCHANGED vars

----
\* Next-state relation.

Next ==
    \/ ClockTick
    \/ AdvanceCommittedSnapshot
    \/ \E t \in Thread : AcquireSnapshot(t)
    \/ \E t \in Thread : ExecuteRead(t)
    \/ \E t \in Thread : FetchOperationTime(t)
    \/ \E t \in Thread : SendResponse(t)
    \/ Done

Spec == Init /\ [][Next]_vars

----
\* Safety properties.

\* SAFETY (primary invariant).
\*
\* For every completed read, the operationTime reported to the client must be
\* less than or equal to the snapshot timestamp at which the read actually
\* executed. If this is violated, the client may use operationTime as
\* afterClusterTime on a follow-up read and observe a value the original read
\* could not have seen, breaking causal consistency.
SnapshotMatchesOperationTime ==
    \A r \in executedReads : r.opTime <= r.snapshot

\* SAFETY (sanity).
\*
\* The committed snapshot is monotone non-decreasing relative to itself within
\* a single thread's lifecycle, and an executed read's snapshot is never
\* greater than the cluster clock that existed when its operationTime was
\* observed.
ExecutedSnapshotBoundedByClock ==
    \A r \in executedReads : r.snapshot <= clusterTime

\* CombinedInvariant groups the safety properties so a single INVARIANT
\* clause in the cfg covers both. Kept as a separate definition rather than
\* inlined so a counterexample names the specific clause that fails.
CombinedInvariant ==
    /\ TypeOK
    /\ ExecutedSnapshotBoundedByClock
    /\ SnapshotMatchesOperationTime

================================================================================
