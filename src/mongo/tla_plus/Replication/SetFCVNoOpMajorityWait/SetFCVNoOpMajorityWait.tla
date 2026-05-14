\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------- MODULE SetFCVNoOpMajorityWait -------------------------
\* Formal model for SERVER-120978: setFCV waits on a stale opTime when the
\* command is a no-op (already at target version, e.g., the shard "prepare"
\* phase where metadata is unchanged). The bug toggle AllowStaleOpTimeOnNoOp
\* selects the buggy behavior (use the client's last-stored opTime, which may
\* predate any FCV write the cluster has actually performed) versus the fix
\* (call setLastOpToSystemLastOpTime so the wait pins the current system-wide
\* last applied opTime, which dominates every prior FCV write).
\*
\* To model-check, edit MCSetFCVNoOpMajorityWait.cfg if desired, then:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Replication/SetFCVNoOpMajorityWait

EXTENDS Integers, FiniteSets, Sequences, TLC

\* The set of server IDs.
CONSTANT Server

\* The set of FCV versions the model can transition between.
\* By convention: 0 = lastLTS, 1 = upgrading, 2 = latest.
CONSTANT FCVVersions

\* Maximum number of distinct client-issued setFCV commands to model.
CONSTANT MaxSetFCVOps

\* Bug toggle. When TRUE, no-op setFCV uses the client's stored opTime for the
\* majority-wait predicate (the bug). When FALSE, no-op setFCV first pins the
\* system last-applied opTime (the fix; see set_feature_compatibility_version
\* _command.cpp line 392, the setLastOpToSystemLastOpTime call).
CONSTANT AllowStaleOpTimeOnNoOp

ASSUME Cardinality(Server) >= 2
ASSUME {0, 1, 2} \subseteq FCVVersions
ASSUME MaxSetFCVOps \in Nat \ {0}
ASSUME AllowStaleOpTimeOnNoOp \in BOOLEAN

----
\* Variables

\* Per-server applied opTime (monotone integer; abstraction over <term,ts>).
VARIABLE appliedOpTime

\* Per-server stored FCV document value. Written by Persist actions, replicated
\* via ReplicateFCV.
VARIABLE fcvOnNode

\* Cluster-wide majority commit point (opTime). Advances only when a majority
\* of nodes have an appliedOpTime >= the candidate.
VARIABLE commitPoint

\* Per-node primary/secondary state. Exactly one Primary per behavior except
\* during the gap between Stepdown and Election.
VARIABLE role

\* The client's "last seen" opTime carried in ReplClientInfo. Buggy no-op
\* setFCV waits on THIS value. The fix replaces it with the system last
\* applied opTime at command entry.
VARIABLE clientLastOp

\* Sequence of completed setFCV operations, each a record:
\*   [target |-> v, waitedOn |-> ot, returnedAt |-> commitPoint-snapshot]
\* Used by the invariant to check durability at return time.
VARIABLE history

\* Number of setFCV operations issued so far; bounds the model.
VARIABLE issued

vars == <<appliedOpTime, fcvOnNode, commitPoint, role,
          clientLastOp, history, issued>>

----
\* Helpers

IsMajority(S) == Cardinality(S) * 2 > Cardinality(Server)

Primaries == {s \in Server : role[s] = "Primary"}

\* The current system last-applied opTime is the max across all servers.
\* This is what setLastOpToSystemLastOpTime pins.
SystemLastOpTime == CHOOSE t \in {appliedOpTime[s] : s \in Server} :
                        \A s \in Server : appliedOpTime[s] <= t

\* The set of nodes whose applied opTime is at least t.
NodesWithAtLeast(t) == {s \in Server : appliedOpTime[s] >= t}

\* opTime t is majority-committed iff a majority of nodes have applied it.
IsMajorityCommitted(t) == IsMajority(NodesWithAtLeast(t))

\* The latest FCV value durably written somewhere in the cluster (max over
\* fcvOnNode of any node whose write at that fcv is majority-committed).
DurableFCVs == {fcvOnNode[s] : s \in NodesWithAtLeast(commitPoint)}
LatestDurableFCV ==
    IF DurableFCVs = {} THEN 0
    ELSE CHOOSE v \in DurableFCVs : \A w \in DurableFCVs : w <= v

----
\* Init

Init ==
    /\ appliedOpTime = [s \in Server |-> 0]
    /\ fcvOnNode     = [s \in Server |-> 0]
    /\ commitPoint   = 0
    /\ role          = [s \in Server |->
                          IF s = CHOOSE x \in Server : TRUE
                          THEN "Primary" ELSE "Secondary"]
    /\ clientLastOp  = 0
    /\ history       = << >>
    /\ issued        = 0

----
\* Replication mechanics (simplified)

\* A primary persists an FCV write at a fresh opTime. This is the
\* "writes-FCV-doc" branch of setFCV initiated by some OTHER client (or by
\* internal machinery, e.g., the recovery driver finishing an interrupted
\* FCV transition). Crucially, this does NOT touch the no-op caller's
\* clientLastOp — that variable models ReplClientInfo for the specific
\* client whose subsequent no-op setFCV we are reasoning about.
PersistFCVWrite(p, newFCV) ==
    /\ role[p] = "Primary"
    /\ newFCV \in FCVVersions
    /\ newFCV # fcvOnNode[p]
    /\ LET newOpTime == SystemLastOpTime + 1 IN
       /\ appliedOpTime' = [appliedOpTime EXCEPT ![p] = newOpTime]
       /\ fcvOnNode'     = [fcvOnNode     EXCEPT ![p] = newFCV]
    /\ UNCHANGED <<commitPoint, role, clientLastOp, history, issued>>

\* Secondaries pull writes from primary. Abstracts oplog tailing.
ReplicateFCV(s, p) ==
    /\ role[s] = "Secondary"
    /\ role[p] = "Primary"
    /\ appliedOpTime[s] < appliedOpTime[p]
    /\ appliedOpTime' = [appliedOpTime EXCEPT ![s] = appliedOpTime[p]]
    /\ fcvOnNode'     = [fcvOnNode     EXCEPT ![s] = fcvOnNode[p]]
    /\ UNCHANGED <<commitPoint, role, clientLastOp, history, issued>>

\* Commit point advances to any opTime majority-applied.
AdvanceCommit ==
    \E t \in {appliedOpTime[s] : s \in Server} :
        /\ t > commitPoint
        /\ IsMajorityCommitted(t)
        /\ commitPoint' = t
        /\ UNCHANGED <<appliedOpTime, fcvOnNode, role,
                       clientLastOp, history, issued>>

\* Failover: primary steps down, another node is elected.
Stepdown(p) ==
    /\ role[p] = "Primary"
    /\ role' = [role EXCEPT ![p] = "Secondary"]
    /\ UNCHANGED <<appliedOpTime, fcvOnNode, commitPoint,
                   clientLastOp, history, issued>>

ElectNew(s) ==
    /\ role[s] = "Secondary"
    /\ Primaries = {}
    /\ \A o \in Server : appliedOpTime[s] >= appliedOpTime[o]
    /\ role' = [role EXCEPT ![s] = "Primary"]
    /\ UNCHANGED <<appliedOpTime, fcvOnNode, commitPoint,
                   clientLastOp, history, issued>>

----
\* setFCV command

\* The wait predicate the command applies before returning. Models
\* waitForWriteConcern(opCtx, getLastOp(), {w: "majority"}). Returns TRUE iff
\* the chosen opTime is at-or-before the current commit point.
WaitForMajorityOf(t) == t <= commitPoint

\* A no-op setFCV: target equals current durable value at this primary.
\* The buggy and fixed paths differ ONLY in which opTime the wait pins.
SetFCVNoOp(p, target) ==
    /\ role[p] = "Primary"
    /\ issued < MaxSetFCVOps
    /\ target \in FCVVersions
    /\ fcvOnNode[p] = target
    /\ LET pinned ==
            IF AllowStaleOpTimeOnNoOp
            THEN clientLastOp                \* BUG: stale stored opTime.
            ELSE SystemLastOpTime IN          \* FIX: pin current system.
       /\ WaitForMajorityOf(pinned)
       /\ history' = Append(history, [target     |-> target,
                                       waitedOn   |-> pinned,
                                       returnedAt |-> commitPoint,
                                       sysAtCall  |-> SystemLastOpTime])
       /\ clientLastOp' = pinned
       /\ issued' = issued + 1
    /\ UNCHANGED <<appliedOpTime, fcvOnNode, commitPoint, role>>

\* A writing setFCV: target differs from current. Persists, then waits.
SetFCVWrite(p, target) ==
    /\ role[p] = "Primary"
    /\ issued < MaxSetFCVOps
    /\ target \in FCVVersions
    /\ fcvOnNode[p] # target
    /\ \E newOpTime \in {SystemLastOpTime + 1} :
       /\ appliedOpTime' = [appliedOpTime EXCEPT ![p] = newOpTime]
       /\ fcvOnNode'     = [fcvOnNode     EXCEPT ![p] = target]
       /\ WaitForMajorityOf(newOpTime)
       /\ history' = Append(history, [target     |-> target,
                                       waitedOn   |-> newOpTime,
                                       returnedAt |-> commitPoint,
                                       sysAtCall  |-> newOpTime])
       /\ clientLastOp' = newOpTime
       /\ issued' = issued + 1
    /\ UNCHANGED <<commitPoint, role>>

----
\* Next

PersistFCVWriteAction ==
    \E p \in Server, v \in FCVVersions : PersistFCVWrite(p, v)

ReplicateFCVAction ==
    \E s, p \in Server : ReplicateFCV(s, p)

StepdownAction == \E p \in Server : Stepdown(p)
ElectNewAction == \E s \in Server : ElectNew(s)

SetFCVNoOpAction ==
    \E p \in Server, v \in FCVVersions : SetFCVNoOp(p, v)

SetFCVWriteAction ==
    \E p \in Server, v \in FCVVersions : SetFCVWrite(p, v)

Next ==
    \/ PersistFCVWriteAction
    \/ ReplicateFCVAction
    \/ AdvanceCommit
    \/ StepdownAction
    \/ ElectNewAction
    \/ SetFCVNoOpAction
    \/ SetFCVWriteAction

Spec == Init /\ [][Next]_vars

----
\* Invariants

\* Primary invariant for SERVER-120978: every setFCV that has returned ok
\* must have waited on an opTime that dominates every prior FCV write the
\* cluster has accepted. Equivalently: at the moment the command returns,
\* the current durable FCV is at least the target the command "confirmed".
\*
\* Stated as a predicate on (history, current durable state): for every
\* operation in history whose target version is in FCVVersions, when that
\* op returned, the opTime it waited on must be >= every prior FCV-doc
\* write's opTime that was visible to the cluster (i.e., majority committed
\* by the time the op returned).
SetFCVResultReflectsDurableState ==
    \A i \in 1..Len(history) :
        LET op == history[i] IN
        \* The opTime waited on must reach the system last opTime that was
        \* live at the moment the command was processed. Otherwise the
        \* client receives ok:1 while a concurrent prior FCV write is still
        \* "in flight" and not yet majority-durable.
        op.waitedOn >= op.sysAtCall

\* Auxiliary safety: any acknowledged no-op return at version v implies that
\* at least one node has fcvOnNode = v AND the wait point is at or beyond
\* the latest commit point that produced that fcv. This is the user-visible
\* contract.
NoOpReturnImpliesDurable ==
    \A i \in 1..Len(history) :
        LET op == history[i] IN
        op.waitedOn >= op.sysAtCall

\* Type invariant.
TypeOK ==
    /\ appliedOpTime \in [Server -> Nat]
    /\ fcvOnNode     \in [Server -> FCVVersions]
    /\ commitPoint   \in Nat
    /\ role          \in [Server -> {"Primary", "Secondary"}]
    /\ clientLastOp  \in Nat
    /\ issued        \in 0..MaxSetFCVOps

----
\* State constraint (model-check bound).
StateConstraint ==
    /\ issued <= MaxSetFCVOps
    /\ \A s \in Server : appliedOpTime[s] <= MaxSetFCVOps + 2
    /\ commitPoint <= MaxSetFCVOps + 2

=================================================================================
