--------------------------------- MODULE RaftMongo ---------------------------------
\* This is the formal specification for the Raft consensus algorithm in MongoDB.

EXTENDS Integers, FiniteSets, Sequences, TLC

\* The set of server IDs.
CONSTANT Server

\* The number of oplog entries that can be created on the primary at one time.
\* For model-checking, this can be 1 or a small number. For model-based trace-
\* checking, set this to the highest observed number of oplog entries that
\* become visible on the primary at one time.
CONSTANT MaxClientWriteSize

----
\* Global variables

\* Whether a client has called "replSetInitiate" on one of the servers.
VARIABLE replSetInitiated

----
\* The following variables are all per server (functions with domain Server).

\* The server's state ("Follower", "Candidate", or "Leader").
VARIABLE state

\* The term learned by each server.
VARIABLE term

\* The commit point learned by each server.
VARIABLE commitPoint

electionVars == <<replSetInitiated, state, term>>
serverVars == <<electionVars, commitPoint>>

\* A Sequence of log entries. The index into this sequence is the index of the
\* log entry. Unfortunately, the Sequence module defines Head(s) as the entry
\* with index 1, so be careful not to use that!
VARIABLE log
logVars == <<log>>

\* End of per server variables.
----

\* All variables; used for stuttering (asserting state hasn't changed).
vars == <<serverVars, logVars>>

----
\* Helpers

\* The set of all quorums. This just calculates simple majorities, but the only
\* important property is that every quorum overlaps with every other.
Quorum == {i \in SUBSET(Server) : Cardinality(i) * 2 > Cardinality(Server)}

\* The term of the last entry in a log, or -1 if the log is empty.
GetTerm(xlog, index) == IF index = 0 THEN -1 ELSE xlog[index].term
LogTerm(i, index) == GetTerm(log[i], index)
LastTerm(xlog) == GetTerm(xlog, Len(xlog))

\* Return the minimum value from a set, or undefined if the set is empty.
Min(s) == CHOOSE x \in s : \A y \in s : x <= y
\* Return the maximum value from a set, or undefined if the set is empty.
Max(s) == CHOOSE x \in s : \A y \in s : x >= y

----
\* Define initial values for all variables

InitServerVars == /\ replSetInitiated  = FALSE
                  /\ state             = [i \in Server |-> "Follower"]
                  /\ term              = [i \in Server |-> -1]
                  /\ commitPoint       = [i \in Server |-> [term |-> -1, index |-> 0]]
InitLogVars == /\ log          = [i \in Server |-> << >>]
Init == /\ InitServerVars
        /\ InitLogVars

----
\* Message handlers
\* i = recipient, j = sender, m = message

\* Receive one or more oplog entries from j, and learn j's term.
AppendOplog(i, j) ==
    /\ Len(log[i]) < Len(log[j])
    /\ LastTerm(log[i]) = LogTerm(j, Len(log[i]))
    /\ \E lastAppended \in (Len(log[i]) + 1)..Len(log[j]):
        LET appendedEntries == SubSeq(log[j], Len(log[i]) + 1, lastAppended)
        IN /\ log' = [log EXCEPT ![i] = log[i] \o appendedEntries]
           /\ term' = [term EXCEPT ![i] = Max({term[i], term[j]})]
    /\ UNCHANGED <<replSetInitiated, state, commitPoint>>

LearnTermViaHeartbeat(i, j) ==
    /\ term' = [term EXCEPT ![i] = Max({term[i], term[j]})]
    /\ UNCHANGED <<replSetInitiated, state, commitPoint, logVars>>

CanRollbackOplog(i, j) ==
    /\ Len(log[i]) > 0
    /\ \* The log with later term is more up-to-date
       LastTerm(log[i]) < LastTerm(log[j])
    /\
       \/ Len(log[i]) > Len(log[j])
       \/ /\ Len(log[i]) <= Len(log[j])
          /\ LastTerm(log[i]) /= LogTerm(j, Len(log[i]))

RollbackOplog(i, j) ==
    /\ CanRollbackOplog(i, j)
    \* Rollback 1 oplog entry
    /\ LET new == [index2 \in 1..(Len(log[i]) - 1) |-> log[i][index2]]
         IN log' = [log EXCEPT ![i] = new]
    /\ UNCHANGED <<serverVars>>

\* The set of nodes that has log[me][logIndex] in their oplog
Agree(me, logIndex) ==
    { node \in Server :
        /\ Len(log[node]) >= logIndex
        /\ LogTerm(me, logIndex) = LogTerm(node, logIndex) }

GlobalCurrentTerm ==
    LET terms == {term[i]: i \in Server}
     IN Max(terms)

IsCommitted(me, logIndex) ==
    /\ Agree(me, logIndex) \in Quorum
    \* Committing log entries in older terms violates the safety properties of the spec.
    \* [ P (2), S (), S ()]
    \* [ S (2), S (), P (3)]
    \* [ S (2), S (2), P (3)] ! the term 2 entry shouldn't be considered committed.
    /\ \E leader \in Server:
        /\ state[leader] = "Leader"
        /\ LogTerm(me, logIndex) = GlobalCurrentTerm

\* RollbackCommitted and NeverRollbackCommitted are not actions.
\* They are used for verification.
RollbackCommitted(i) ==
    \E j \in Server:
        /\ CanRollbackOplog(i, j)
        /\ IsCommitted(i, Len(log[i]))

NeverRollbackCommitted ==
    \A i \in Server: ~RollbackCommitted(i)

\* ACTION
\* Follower i receives replSetInitiate and writes the first oplog entry, which is a no-op.
\* Not needed for correctness, but modeled here to match the implementation.
ReplSetInitiate(i) ==
    /\ Init
    /\ LET entry == [term |-> -1]
       IN /\ replSetInitiated' = TRUE
          /\ log' = [log EXCEPT ![i] = Append(log[i], entry)]
    /\ UNCHANGED <<state, term, commitPoint>>

\* ACTION
\* i = the new primary node.
\* In the implementation, term starts at -1, then 1, then increments normally.
BecomePrimaryByMagic(i) ==
    LET notBehind(me, j) ==
            \/ LastTerm(log[me]) > LastTerm(log[j])
            \/ /\ LastTerm(log[me]) = LastTerm(log[j])
               /\ Len(log[me]) >= Len(log[j])
        ayeVoters(me) ==
            { index \in Server : notBehind(me, index) }
        nextTerm == IF GlobalCurrentTerm = -1 THEN 1 ELSE GlobalCurrentTerm + 1
    IN /\ ayeVoters(i) \in Quorum
       /\ state' = [index \in Server |-> IF index = i THEN "Leader" ELSE "Follower"]
       /\ term' = [term EXCEPT ![i] = nextTerm]
       /\ UNCHANGED <<replSetInitiated, commitPoint, logVars>>

\* ACTION
\* Leader i receives a client request to add one or more entries to the log.
ClientWrite(i) ==
    /\ state[i] = "Leader"
    /\ \E numEntries \in 1..MaxClientWriteSize :
        LET entry == [term |-> term[i]]
            newEntries == [ j \in 1..numEntries |-> entry ]
            newLog == log[i] \o newEntries
        IN  log' = [log EXCEPT ![i] = newLog]
    /\ UNCHANGED <<replSetInitiated, serverVars>>

\* ACTION
AdvanceCommitPoint ==
    \E leader \in Server :
        /\ state[leader] = "Leader"
        \* New commitPoint is any committed log index after current commitPoint
        /\ \E committedIndex \in (commitPoint[leader].index+1)..Len(log[leader]) :
            /\ IsCommitted(leader, committedIndex)
            /\ LET newCommitPoint == [term |-> LogTerm(leader, committedIndex), index |-> committedIndex]
               IN  commitPoint' = [commitPoint EXCEPT ![leader] = newCommitPoint]
            /\ UNCHANGED <<electionVars, logVars>>

\* Return whether Node i can learn the commit point from Node j.
CommitPointLessThan(i, j) ==
   \/ commitPoint[i].term < commitPoint[j].term
   \/ /\ commitPoint[i].term = commitPoint[j].term
      /\ commitPoint[i].index < commitPoint[j].index

\* ACTION
LearnCommitPointFromSyncSourceNeverBeyondLastApplied(i, j) ==
    \* From sync source
    /\ ENABLED AppendOplog(i, j)
    /\ CommitPointLessThan(i, j)
    \* Never beyond last applied
    /\ LET myCommitPoint ==
            \* If they have the same term, commit point can be ahead.
            IF commitPoint[j].term <= LastTerm(log[i])
            THEN commitPoint[j]
            ELSE [term |-> LastTerm(log[i]), index |-> Len(log[i])]
       IN commitPoint' = [commitPoint EXCEPT ![i] = myCommitPoint]
    /\ UNCHANGED <<electionVars, logVars>>

----
AppendOplogAction ==
    \E i,j \in Server : AppendOplog(i, j)

LearnTermViaHeartbeatAction ==
    \E i,j \in Server : LearnTermViaHeartbeat(i, j)
    
RollbackOplogAction ==
    \E i,j \in Server : RollbackOplog(i, j)

BecomePrimaryByMagicAction ==
    \E i \in Server : BecomePrimaryByMagic(i)

ReplSetInitiateAction ==
    \E i \in Server : ReplSetInitiate(i)

ClientWriteAction ==
    \E i \in Server : ClientWrite(i)

LearnCommitPointFromSyncSourceNeverBeyondLastAppliedAction ==
    \E i, j \in Server : LearnCommitPointFromSyncSourceNeverBeyondLastApplied(i, j)

----
\* Properties to check

RollbackBeforeCommitPoint(i) ==
    /\ \E j \in Server:
        /\ CanRollbackOplog(i, j)
    /\ \/ LastTerm(log[i]) < commitPoint[i].term
       \/ /\ LastTerm(log[i]) = commitPoint[i].term
          /\ Len(log[i]) <= commitPoint[i].index

NeverRollbackBeforeCommitPoint == \A i \in Server: ~RollbackBeforeCommitPoint(i)

\* Liveness check

\* This isn't accurate for any infinite behavior specified by Spec, but it's fine
\* for any finite behavior with the liveness we can check with the model checker.
\* This is to check at any time, if two nodes' commit points are not the same, they
\* will be the same eventually.
\* This is checked after all possible rollback is done.
CommitPointEventuallyPropagates ==
    /\ \A i, j \in Server:
        [](commitPoint[i] # commitPoint[j] ~>
               <>(~ENABLED RollbackOplogAction => commitPoint[i] = commitPoint[j]))

----
\* Defines how the variables may transition.
\*
\* MongoDB's commit point learning protocol has evolved as we discovered
\* protocol bugs, see:
\*
\* https://conf.tlapl.us/07_-_TLAConf19_-_William_Schultz_-_Fixing_a_MongoDB_Replication_Protocol_Bug_with_TLA.pdf
\*
Next ==
    \* --- Replication protocol
    \/ AppendOplogAction
    \/ LearnTermViaHeartbeatAction
    \/ RollbackOplogAction
    \/ BecomePrimaryByMagicAction
    \/ ReplSetInitiateAction
    \/ ClientWriteAction
    \*
    \* --- Commit point learning protocol
    \/ AdvanceCommitPoint
    \/ LearnCommitPointFromSyncSourceNeverBeyondLastAppliedAction

Safety == Init /\ [][Next]_vars

\* Comment or uncomment the commit point learning actions here to match those in "Next".
Liveness ==
    /\ SF_vars(AppendOplogAction)
    /\ SF_vars(LearnTermViaHeartbeatAction)
    /\ SF_vars(RollbackOplogAction)
    \* A new primary should eventually write one entry.
    /\ WF_vars(\E i \in Server : LastTerm(log[i]) # GlobalCurrentTerm /\ ClientWrite(i))
    \* /\ WF_vars(ClientWriteAction)
    \*
    /\ WF_vars(AdvanceCommitPoint)
    /\ SF_vars(LearnCommitPointFromSyncSourceNeverBeyondLastAppliedAction)

\* The specification must start with the initial state and transition according
\* to Next.
Spec == Safety /\ Liveness

===============================================================================
