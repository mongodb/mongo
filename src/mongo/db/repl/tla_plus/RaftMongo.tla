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
\* The following variables are all per server (functions with domain Server).

\* The server's state ("Follower", "Candidate", or "Leader").
VARIABLE state

\* The term learned by each server.
VARIABLE term

\* The commit point learned by each server.
VARIABLE commitPoint

electionVars == <<state, term>>
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

\* The term of the last entry in a log, or 0 if the log is empty.
GetTerm(xlog, index) == IF index = 0 THEN 0 ELSE xlog[index].term
LogTerm(i, index) == GetTerm(log[i], index)
LastTerm(xlog) == GetTerm(xlog, Len(xlog))

\* Server i is allowed to sync from server j.
CanSyncFrom(i, j) ==
    /\ Len(log[i]) <= Len(log[j])
    /\ LastTerm(log[i]) = LogTerm(j, Len(log[i]))

\* Return the minimum value from a set, or undefined if the set is empty.
Min(s) == CHOOSE x \in s : \A y \in s : x <= y
\* Return the maximum value from a set, or undefined if the set is empty.
Max(s) == CHOOSE x \in s : \A y \in s : x >= y

GlobalCurrentTerm ==
    LET terms == {term[i]: i \in Server}
     IN Max(terms)

\* The set of nodes that has log[me][logIndex] in their oplog
Agree(me, logIndex) ==
    { node \in Server :
        /\ Len(log[node]) >= logIndex
        /\ LogTerm(me, logIndex) = LogTerm(node, logIndex) }

\* Return whether Node i can learn the commit point from Node j.
CommitPointLessThan(i, j) ==
   \/ commitPoint[i].term < commitPoint[j].term
   \/ /\ commitPoint[i].term = commitPoint[j].term
      /\ commitPoint[i].index < commitPoint[j].index

IsCommitted(me, logIndex) ==
    /\ Agree(me, logIndex) \in Quorum
    \* Committing log entries in older terms violates the safety properties of the spec.
    \* [ P (2), S (), S ()]
    \* [ S (2), S (), P (3)]
    \* [ S (2), S (2), P (3)] ! the term 2 entry shouldn't be considered committed.
    /\ \E leader \in Server:
        /\ state[leader] = "Leader"
        /\ LogTerm(me, logIndex) = GlobalCurrentTerm

CanRollbackOplog(i, j) ==
    /\ Len(log[i]) > 0
    /\ \* The log with later term is more up-to-date
       LastTerm(log[i]) < LastTerm(log[j])
    /\
       \/ Len(log[i]) > Len(log[j])
       \/ /\ Len(log[i]) <= Len(log[j])
          /\ LastTerm(log[i]) /= LogTerm(j, Len(log[i]))

RollbackCommitted(i) ==
    \E j \in Server:
        /\ CanRollbackOplog(i, j)
        /\ IsCommitted(i, Len(log[i]))

----
\* Define initial values for all variables

InitServerVars == /\ state             = [i \in Server |-> "Follower"]
                  /\ term              = [i \in Server |-> 0]
                  /\ commitPoint       = [i \in Server |-> [term |-> 0, index |-> 0]]
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
    /\ UNCHANGED <<state, commitPoint>>

LearnTermViaHeartbeat(i, j) ==
    /\ term' = [term EXCEPT ![i] = Max({term[i], term[j]})]
    /\ UNCHANGED <<state, commitPoint, logVars>>

\* Node i learns the commit point from j via heartbeat.
LearnCommitPoint(i, j) ==
    /\ CommitPointLessThan(i, j)
    /\ commitPoint' = [commitPoint EXCEPT ![i] = commitPoint[j]]
    /\ UNCHANGED <<electionVars, logVars>>

RollbackOplog(i, j) ==
    /\ CanRollbackOplog(i, j)
    \* Rollback 1 oplog entry
    /\ LET new == [index2 \in 1..(Len(log[i]) - 1) |-> log[i][index2]]
         IN log' = [log EXCEPT ![i] = new]
    /\ UNCHANGED <<serverVars>>

\* ACTION
\* i = the new primary node.
BecomePrimaryByMagic(i) ==
    LET notBehind(me, j) ==
            \/ LastTerm(log[me]) > LastTerm(log[j])
            \/ /\ LastTerm(log[me]) = LastTerm(log[j])
               /\ Len(log[me]) >= Len(log[j])
        ayeVoters(me) ==
            { index \in Server : notBehind(me, index) }
    IN /\ ayeVoters(i) \in Quorum
       /\ state' = [index \in Server |-> IF index = i THEN "Leader" ELSE "Follower"]
       /\ term' = [term EXCEPT ![i] = GlobalCurrentTerm + 1]
       /\ UNCHANGED <<commitPoint, logVars>>

\* ACTION
\* Leader i receives a client request to add one or more entries to the log.
ClientWrite(i) ==
    /\ state[i] = "Leader"
    /\ \E numEntries \in 1..MaxClientWriteSize :
        LET entry == [term |-> term[i]]
            newEntries == [ j \in 1..numEntries |-> entry ]
            newLog == log[i] \o newEntries
        IN  log' = [log EXCEPT ![i] = newLog]
    /\ UNCHANGED <<serverVars>>

\* ACTION
AdvanceCommitPoint ==
    \E leader \in Server :
        /\ state[leader] = "Leader"
        \* New commitPoint is any committed log index after current commitPoint
        /\ \E committedIndex \in (commitPoint[leader].index + 1)..Len(log[leader]) :
            /\ IsCommitted(leader, committedIndex)
            /\ LET newCommitPoint == [term |-> LogTerm(leader, committedIndex), index |-> committedIndex]
               IN  commitPoint' = [commitPoint EXCEPT ![leader] = newCommitPoint]
            /\ UNCHANGED <<electionVars, logVars>>

\* ACTION
\* Node i learns the commit point from j via heartbeat with term check
LearnCommitPointWithTermCheck(i, j) ==
    /\ LastTerm(log[i]) = commitPoint[j].term
    /\ LearnCommitPoint(i, j)

\* ACTION
LearnCommitPointFromSyncSourceNeverBeyondLastApplied(i, j) ==
    \* From sync source
    /\ CanSyncFrom(i, j)
    /\ CommitPointLessThan(i, j)
    \* Never beyond last applied
    /\ LET myCommitPoint ==
            \* If j's term is less than or equal to i's, commit point can be ahead.
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

ClientWriteAction ==
    \E i \in Server : ClientWrite(i)

LearnCommitPointWithTermCheckAction ==
    \E i, j \in Server : LearnCommitPointWithTermCheck(i, j)

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
    \/ ClientWriteAction
    \*
    \* --- Commit point learning protocol
    \/ AdvanceCommitPoint
    \/ LearnCommitPointWithTermCheckAction
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
    /\ SF_vars(LearnCommitPointWithTermCheckAction)
    /\ SF_vars(LearnCommitPointFromSyncSourceNeverBeyondLastAppliedAction)

\* The specification must start with the initial state and transition according
\* to Next.
Spec == Safety /\ Liveness

\* Invariant for model-checking
NeverRollbackCommitted ==
    \A i \in Server: ~RollbackCommitted(i)

===============================================================================
