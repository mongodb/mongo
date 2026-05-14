\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------------- MODULE FLE2StmtIdContract ----------------------------------
\* Models the stmtId assignment behavior of FLE2/QE batched inserts inside a retryable
\* internal transaction. SERVER-79952: FLE2 batched inserts only respect the stmtId of the
\* first statement of an insert request and then increment it freely for each generated
\* auxiliary write, rather than tagging auxiliary writes with kUninitializedStmtId (-1) as
\* required by the retryable internal transactions contract documented in
\* src/mongo/db/s/README_sessions_and_transactions.md.
\*
\* The contract being checked:
\*   (C1) Every client write statement appears with its caller-supplied stmtId in the
\*        retryable history, exactly once.
\*   (C2) Every auxiliary write (ESC/ECOC metadata, __safeContent__ pull-update, etc.)
\*        appears in the retryable history with stmtId = kUninitializedStmtId.
\*   (C3) Distinct client statements never collide on the same stmtId.
\*
\* This spec models two implementations side-by-side:
\*   BuggyAssign  -- current production behavior (increments baseStmtId for every aux write).
\*   FixedAssign  -- proposed behavior (aux writes use kUninitializedStmtId, client writes
\*                   use the caller-supplied stmtIds verbatim).
\*
\* The configured CONSTANT Mode selects which implementation drives Next so a single TLC
\* run can exhibit a counterexample under Mode = "buggy" and pass under Mode = "fixed".

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS ClientStmtIds,  \* Sequence of caller-supplied stmtIds for the batched insert,
                          \* e.g. <<1, 3>> for op1 with stmtId 1 and op2 with stmtId 3.
          AuxPerOp,       \* Number of auxiliary writes emitted per client op (ESC + ECOC
                          \* + optional __safeContent__ pull-update). Modeled as a
                          \* per-op count rather than per-tag count.
          Mode            \* "buggy" or "fixed".

\* Sentinel for unassigned/auxiliary stmtId. Matches kUninitializedStmtId in the server.
UninitializedStmtId == -1

\* Op kinds.
Client == "client"
Aux    == "aux"

ASSUME /\ Mode \in {"buggy", "fixed"}
       /\ AuxPerOp \in Nat
       /\ Len(ClientStmtIds) >= 1
       /\ \A i \in 1..Len(ClientStmtIds) : ClientStmtIds[i] \in Nat /\ ClientStmtIds[i] >= 0
       \* Caller-supplied stmtIds are distinct (drivers contract).
       /\ \A i, j \in 1..Len(ClientStmtIds) : i # j => ClientStmtIds[i] # ClientStmtIds[j]

VARIABLES
    nextOp,        \* Index of the next client op to process (1..Len(ClientStmtIds)+1).
    baseStmtId,    \* "Live" stmtId cursor used by the buggy assigner.
    history        \* Sequence of [kind, stmtId, opIndex] records modeling the retryable
                   \* history that the txn participant would observe.

vars == <<nextOp, baseStmtId, history>>

NumOps == Len(ClientStmtIds)

-----------------------------------------------------------------------------
\* Helpers
-----------------------------------------------------------------------------

\* Concatenation: TLA+ sequences support \o directly, so just alias it.
AppendEntries(s, entries) == s \o entries

\* Buggy: client stmtId comes from the running cursor (which only matches the user-supplied
\* value for op1); every aux write also bumps the cursor.
BuggyEntriesForOp(i, cursor) ==
    LET clientEntry == [kind |-> Client, stmtId |-> cursor, opIndex |-> i]
        auxEntries  == [k \in 1..AuxPerOp |->
                          [kind |-> Aux, stmtId |-> cursor + k, opIndex |-> i]]
    IN <<clientEntry>> \o auxEntries

BuggyCursorAfterOp(cursor) == cursor + 1 + AuxPerOp

\* Fixed: client stmtId is the caller-supplied stmtId verbatim; aux writes carry
\* kUninitializedStmtId so the participant excludes them from the retryable history.
FixedEntriesForOp(i) ==
    LET clientEntry == [kind |-> Client, stmtId |-> ClientStmtIds[i], opIndex |-> i]
        auxEntries  == [k \in 1..AuxPerOp |->
                          [kind |-> Aux, stmtId |-> UninitializedStmtId, opIndex |-> i]]
    IN <<clientEntry>> \o auxEntries

-----------------------------------------------------------------------------
\* Actions
-----------------------------------------------------------------------------

Init ==
    /\ nextOp = 1
    /\ baseStmtId = ClientStmtIds[1]
    /\ history = <<>>

ProcessOpBuggy ==
    /\ Mode = "buggy"
    /\ nextOp <= NumOps
    /\ history' = AppendEntries(history, BuggyEntriesForOp(nextOp, baseStmtId))
    /\ baseStmtId' = BuggyCursorAfterOp(baseStmtId)
    /\ nextOp' = nextOp + 1

ProcessOpFixed ==
    /\ Mode = "fixed"
    /\ nextOp <= NumOps
    /\ history' = AppendEntries(history, FixedEntriesForOp(nextOp))
    /\ baseStmtId' = baseStmtId
    /\ nextOp' = nextOp + 1

\* Explicit terminal-state stutter so TLC doesn't report a (real) deadlock when both
\* ProcessOp actions become disabled. Model-check.sh does not pass -deadlock.
Done ==
    /\ nextOp > NumOps
    /\ UNCHANGED vars

Next ==
    \/ ProcessOpBuggy
    \/ ProcessOpFixed
    \/ Done

Spec ==
    /\ Init
    /\ [][Next]_vars
    /\ WF_vars(ProcessOpBuggy)
    /\ WF_vars(ProcessOpFixed)

-----------------------------------------------------------------------------
\* Invariants
-----------------------------------------------------------------------------

TypeOK ==
    /\ nextOp \in 1..(NumOps + 1)
    /\ baseStmtId \in Int
    /\ \A i \in 1..Len(history) :
        /\ history[i].kind \in {Client, Aux}
        /\ history[i].stmtId \in Int
        /\ history[i].opIndex \in 1..NumOps

ClientEntries == { i \in 1..Len(history) : history[i].kind = Client }
AuxEntries    == { i \in 1..Len(history) : history[i].kind = Aux }

\* (C1) Every client op appears exactly once with its caller-supplied stmtId.
ClientStmtIdsPreserved ==
    nextOp > NumOps =>
        \A i \in 1..NumOps :
            \E j \in ClientEntries :
                /\ history[j].opIndex = i
                /\ history[j].stmtId = ClientStmtIds[i]

\* (C2) Every auxiliary write is tagged kUninitializedStmtId.
AuxStmtIdsUninitialized ==
    \A j \in AuxEntries : history[j].stmtId = UninitializedStmtId

\* (C3) Distinct client entries never collide on the same stmtId. (Independent check —
\* derivable from C1 but pinned here so a counterexample names the collision directly.)
ClientStmtIdsUnique ==
    \A i, j \in ClientEntries :
        i # j => history[i].stmtId # history[j].stmtId

\* (C4) Auxiliary writes never silently overwrite a caller-supplied stmtId. This is the
\* property the bug violates: with ClientStmtIds = <<1, 3>> and AuxPerOp = 1, the buggy
\* assigner produces aux entries at stmtId = 2 and client-op-2 at stmtId = 3, but it also
\* produces an aux entry at stmtId = 3 inside op-1 (when AuxPerOp >= 2), colliding with
\* client-op-2.
NoAuxOverwritesClientStmtId ==
    \A i \in AuxEntries :
        history[i].stmtId = UninitializedStmtId
        \/ ~ \E k \in 1..NumOps : ClientStmtIds[k] = history[i].stmtId

-----------------------------------------------------------------------------
\* Liveness
-----------------------------------------------------------------------------

\* Every batched insert eventually finishes processing all client ops.
AllOpsEventuallyProcessed == <>(nextOp > NumOps)

=================================================================================================
