\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------------- MODULE PreparedTxnStartupRecovery ------------------------------
\* SERVER-115355: Recover prepared transactions at startup.
\*
\* SERVER-113729 made it possible to recover prepared transactions from a precise checkpoint,
\* but only after the node had transitioned to PRIMARY (to work around WT-15051 / WT-16600).
\* This spec models the new path: a node discovers and re-prepares its in-progress prepared
\* transactions during startup recovery, before any state transition, then replays the tail of
\* the oplog beyond the stable checkpoint. A subsequent commit or abort entry in the oplog tail
\* (or issued by a client once the node is steady-state) must deterministically resolve the
\* prepared transaction.
\*
\* The spec abstracts a single replica-set node across a sequence of crash/restart cycles. A
\* transaction's lifecycle is: NotStarted -> InProgress -> Prepared -> {Committed, Aborted}.
\* The "stable checkpoint" is the snapshot the node persists before each crash; the "oplog
\* tail" is the suffix of the oplog beyond that checkpoint that startup recovery must replay.
\*
\* To run the model-checker:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Transactions/PreparedTxnStartupRecovery

EXTENDS Integers, FiniteSets, Sequences, TLC

CONSTANTS Txns,            \* Set of transaction identifiers.
          MaxRestarts,     \* Bound on the number of crash/restart cycles.
          MaxOplogTail     \* Bound on the length of oplog beyond the stable checkpoint.

\* Transaction states.
NotStarted == "NotStarted"
InProgress == "InProgress"
Prepared   == "Prepared"
Committed  == "Committed"
Aborted    == "Aborted"

States == {NotStarted, InProgress, Prepared, Committed, Aborted}

\* Oplog entry kinds we model.
OpInsert  == "insert"
OpPrepare == "prepare"
OpCommit  == "commit"
OpAbort   == "abort"

OpKinds == {OpInsert, OpPrepare, OpCommit, OpAbort}

VARIABLES
    \* In-memory transaction state, keyed by txn id.
    txnState,
    \* Oplog: a sequence of records [txn |-> t, kind |-> k]. Append-only between restarts.
    oplog,
    \* Index into oplog of the last entry included in the stable checkpoint. Entries with
    \* index > stableIdx are the "oplog tail" that startup recovery must replay.
    stableIdx,
    \* The persisted state of each transaction as of the stable checkpoint. This is what
    \* startup recovery rebuilds in-memory state from before replaying the tail.
    checkpointState,
    \* Number of crash/restart cycles that have occurred.
    restartCount,
    \* TRUE iff the node is currently recovering (between crash and end of tail replay).
    recovering,
    \* History of resolved (committed or aborted) txns, used by invariants.
    resolved

vars == <<txnState, oplog, stableIdx, checkpointState,
          restartCount, recovering, resolved>>

----
\* Helpers.

EmptyState == [t \in Txns |-> NotStarted]

\* Replay one oplog entry against an in-memory state map.
ApplyEntry(stateMap, entry) ==
    LET t == entry.txn IN
    CASE entry.kind = OpInsert  -> [stateMap EXCEPT ![t] = InProgress]
      [] entry.kind = OpPrepare -> [stateMap EXCEPT ![t] = Prepared]
      [] entry.kind = OpCommit  -> [stateMap EXCEPT ![t] = Committed]
      [] entry.kind = OpAbort   -> [stateMap EXCEPT ![t] = Aborted]

\* Recursive replay of a sub-sequence of the oplog starting from stateMap.
RECURSIVE Replay(_, _, _, _)
Replay(stateMap, log, from, to) ==
    IF from > to THEN stateMap
    ELSE Replay(ApplyEntry(stateMap, log[from]), log, from + 1, to)

\* The transactions that are prepared as of the stable checkpoint. Startup recovery is
\* responsible for re-establishing the in-memory prepared state for every such txn before
\* the node accepts new client traffic or transitions out of STARTUP.
PreparedAtCheckpoint == {t \in Txns : checkpointState[t] = Prepared}

\* Final state we expect after replaying the entire oplog from EmptyState.
ExpectedState == Replay(EmptyState, oplog, 1, Len(oplog))

CanAppend(t, kind) ==
    CASE kind = OpInsert  -> txnState[t] = NotStarted
      [] kind = OpPrepare -> txnState[t] = InProgress
      [] kind = OpCommit  -> txnState[t] = Prepared
      [] kind = OpAbort   -> txnState[t] \in {InProgress, Prepared}

OplogTailLen == Len(oplog) - stableIdx

----
\* Initial state: empty oplog, empty checkpoint, no restarts yet, not recovering.
Init ==
    /\ txnState        = EmptyState
    /\ oplog           = << >>
    /\ stableIdx       = 0
    /\ checkpointState = EmptyState
    /\ restartCount    = 0
    /\ recovering      = FALSE
    /\ resolved        = {}

----
\* Actions.

\* Append a new oplog entry while the node is steady-state. Models client writes,
\* prepareTransaction, commitTransaction, abortTransaction.
AppendEntry(t, kind) ==
    /\ ~recovering
    /\ kind \in OpKinds
    /\ OplogTailLen < MaxOplogTail
    /\ CanAppend(t, kind)
    /\ oplog' = Append(oplog, [txn |-> t, kind |-> kind])
    /\ txnState' = ApplyEntry(txnState, [txn |-> t, kind |-> kind])
    /\ resolved' = IF kind \in {OpCommit, OpAbort} THEN resolved \union {t} ELSE resolved
    /\ UNCHANGED <<stableIdx, checkpointState, restartCount, recovering>>

\* Take a stable checkpoint of the current state. Advances stableIdx to the current oplog
\* length and snapshots txnState into checkpointState.
TakeStableCheckpoint ==
    /\ ~recovering
    /\ stableIdx < Len(oplog)
    /\ stableIdx' = Len(oplog)
    /\ checkpointState' = txnState
    /\ UNCHANGED <<txnState, oplog, restartCount, recovering, resolved>>

\* Crash: discard volatile in-memory state. Oplog and stable checkpoint persist; the oplog
\* tail beyond the checkpoint also persists on disk and will be replayed by recovery.
Crash ==
    /\ ~recovering
    /\ restartCount < MaxRestarts
    /\ recovering' = TRUE
    /\ txnState' = EmptyState
    /\ restartCount' = restartCount + 1
    /\ UNCHANGED <<oplog, stableIdx, checkpointState, resolved>>

\* SERVER-115355 core action: during STARTUP, rebuild in-memory state from the stable
\* checkpoint and replay the oplog tail. This must re-establish the Prepared state for
\* every transaction that was Prepared at the checkpoint and not subsequently resolved in
\* the tail. Before SERVER-115355 this work could only be done after PRIMARY transition;
\* after the change it happens during startup.
StartupRecover ==
    /\ recovering
    /\ txnState' = Replay(checkpointState, oplog, stableIdx + 1, Len(oplog))
    /\ recovering' = FALSE
    /\ UNCHANGED <<oplog, stableIdx, checkpointState, restartCount, resolved>>

Next ==
    \/ \E t \in Txns, k \in OpKinds : AppendEntry(t, k)
    \/ TakeStableCheckpoint
    \/ Crash
    \/ StartupRecover

Spec ==
    /\ Init
    /\ [][Next]_vars
    /\ WF_vars(StartupRecover)

----
\* Invariants.

TypeOK ==
    /\ txnState        \in [Txns -> States]
    /\ checkpointState \in [Txns -> States]
    /\ stableIdx \in 0..Len(oplog)
    /\ restartCount \in 0..MaxRestarts
    /\ recovering \in BOOLEAN
    /\ resolved \subseteq Txns
    /\ \A i \in 1..Len(oplog) :
        /\ oplog[i].txn  \in Txns
        /\ oplog[i].kind \in OpKinds

\* The checkpoint can never reference an oplog index beyond the oplog itself.
CheckpointInOplog == stableIdx <= Len(oplog)

\* Once a transaction has been committed, it cannot move back to any earlier state.
\* Once aborted, likewise. (Modeled at the oplog level: replay is monotonic past commit/abort.)
NoResurrection ==
    \A t \in Txns :
        (t \in resolved) =>
            (ExpectedState[t] = Committed \/ ExpectedState[t] = Aborted)

\* The headline correctness property for SERVER-115355: once startup recovery completes
\* (recovering = FALSE), the in-memory state must match what you would get by replaying
\* the entire oplog from scratch. In particular, every prepared-at-checkpoint txn that was
\* not resolved in the tail is back in the Prepared state in memory.
RecoveryReconstructsPreparedState ==
    (~recovering) => (txnState = ExpectedState)

\* During steady state, a transaction is Prepared in memory iff the oplog says so.
PreparedStateMatchesOplog ==
    (~recovering) =>
        \A t \in Txns :
            (txnState[t] = Prepared) <=> (ExpectedState[t] = Prepared)

\* Every txn that was Prepared at the checkpoint is either still Prepared after recovery,
\* or has been explicitly resolved by an entry in the oplog tail. There is no third option
\* like "silently lost" or "rolled back to InProgress".
PreparedTxnsNeverLost ==
    (~recovering) =>
        \A t \in PreparedAtCheckpoint :
            txnState[t] \in {Prepared, Committed, Aborted}

\* The set of resolved txns matches exactly those that reached Committed or Aborted in the
\* oplog. This pins down idempotent replay: replaying twice doesn't fabricate a resolution.
ResolvedMatchesOplog ==
    resolved = {t \in Txns : ExpectedState[t] \in {Committed, Aborted}}

================================================================================================
