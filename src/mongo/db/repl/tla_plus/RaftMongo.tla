\* Copyright 2019 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

--------------------------------- MODULE RaftMongo ---------------------------------
\* This is the formal specification for the Raft consensus algorithm in MongoDB.

\* INSTRUCTIONS FOR MODEL-CHECKING IN THE TLA+ TOOLBOX
\* "What is the behavior spec?"
\*     Temporal formula: Spec
\* "What is the model?"
\*     "Specify the value of declared constants"
\*         MaxClientWriteSize = small number like 2 to limit state space
\*         Servers = a set of your desired replica set size, e.g. {1, 2, 3}
\* "What to check?"
\*     Deadlock: checked
\*     Invariants: NeverRollbackCommitted
\* "Additional Spec Options"
\*     "State Constraint"
\*         Add a state constraint to limit the state space like:
\*             /\ globalCurrentTerm <= 3
\*             /\ \forall i \in Server: Len(log[i]) <= 5

EXTENDS Integers, FiniteSets, Sequences, TLC

\* The set of server IDs.
CONSTANT Server

\* The maximum number of oplog entries that can be created on the primary in one
\* action. For model-checking, this can be 1 or a small number.
CONSTANT MaxClientWriteSize

\* The maximum term known by any server.
VARIABLE globalCurrentTerm

----
\* The following variables are all per server (functions with domain Server).

\* The server's state ("Follower", "Candidate", or "Leader").
VARIABLE state

\* The commit point learned by each server.
VARIABLE commitPoint

electionVars == <<globalCurrentTerm, state>>
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

IsMajority(servers) == Cardinality(servers) * 2 > Cardinality(Server)
GetTerm(xlog, index) == IF index = 0 THEN 0 ELSE xlog[index].term
LogTerm(i, index) == GetTerm(log[i], index)
LastTerm(xlog) == GetTerm(xlog, Len(xlog))

\* Return the maximum value from a set, or undefined if the set is empty.
Max(s) == CHOOSE x \in s : \A y \in s : x >= y

\* Server i is allowed to sync from server j.
CanSyncFrom(i, j) ==
    /\ Len(log[i]) < Len(log[j])
    /\ LastTerm(log[i]) = LogTerm(j, Len(log[i]))

\* Server "me" is ahead of or caught up to server j.
NotBehind(me, j) == \/ LastTerm(log[me]) > LastTerm(log[j])
                    \/ /\ LastTerm(log[me]) = LastTerm(log[j])
                       /\ Len(log[me]) >= Len(log[j])

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
    /\ IsMajority(Agree(me, logIndex))
    /\ LogTerm(me, logIndex) = globalCurrentTerm

\* Is it possible for node i's log to roll back based on j's log? If true, it
\* implies that i's log should remove entries to become a prefix of j's.
CanRollbackOplog(i, j) ==
    /\ Len(log[i]) > 0
    /\ Len(log[j]) > 0
    \* The terms of the last entries of each log do not match. The term of node
    \* j's last log entry is greater than that of node i's.
    /\ LastTerm(log[j]) > LastTerm(log[i])

RollbackCommitted(i) ==
    \E j \in Server:
        /\ CanRollbackOplog(i, j)
        /\ IsCommitted(i, Len(log[i]))

----
\* Define initial values for all variables

InitServerVars == /\ globalCurrentTerm = 0
                  /\ state             = [i \in Server |-> "Follower"]
                  /\ commitPoint       = [i \in Server |-> [term |-> 0, index |-> 0]]
InitLogVars ==    /\ log               = [i \in Server |-> << >>]
Init == /\ InitServerVars
        /\ InitLogVars

----
\* Message handlers
\* i = recipient, j = sender

\* Receive one or more oplog entries from j.
AppendOplog(i, j) ==
    /\ CanSyncFrom(i, j)
    /\ \E lastAppended \in (Len(log[i]) + 1)..Len(log[j]):
        LET appendedEntries == SubSeq(log[j], Len(log[i]) + 1, lastAppended)
         IN log' = [log EXCEPT ![i] = log[i] \o appendedEntries]
    /\ UNCHANGED <<serverVars>>

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
\* Node i is elected by a majority, and nodes that voted for it can't still be primary.
\* A stale primary might persist among the minority that didn't vote for it.
BecomePrimaryByMagic(i, ayeVoters) ==
    /\ \A j \in ayeVoters : NotBehind(i, j)
    /\ IsMajority(ayeVoters)
    /\ state' = [index \in Server |-> IF index \notin ayeVoters
                                      THEN state[index]
                                      ELSE IF index = i THEN "Leader" ELSE "Follower"]
    /\ globalCurrentTerm' = globalCurrentTerm + 1
    /\ UNCHANGED <<commitPoint, logVars>>

\* ACTION
\* Leader i receives a client request to add one or more entries to the log.
ClientWrite(i) ==
    /\ state[i] = "Leader"
    /\ \E numEntries \in 1..MaxClientWriteSize :
        LET entry == [term |-> globalCurrentTerm]
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
            /\ LET newCommitPoint == [
                       term |-> LogTerm(leader, committedIndex),
                       index |-> committedIndex
                   ]
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

RollbackOplogAction ==
    \E i,j \in Server : RollbackOplog(i, j)

BecomePrimaryByMagicAction ==
    \E i \in Server : \E ayeVoters \in SUBSET(Server) : BecomePrimaryByMagic(i, ayeVoters)

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
    \/ RollbackOplogAction
    \/ BecomePrimaryByMagicAction
    \/ ClientWriteAction
    \*
    \* --- Commit point learning protocol
    \/ AdvanceCommitPoint
    \/ LearnCommitPointWithTermCheckAction
    \/ LearnCommitPointFromSyncSourceNeverBeyondLastAppliedAction

SpecBehavior == Init /\ [][Next]_vars

Liveness ==
    /\ SF_vars(AppendOplogAction)
    /\ SF_vars(RollbackOplogAction)
    \* A new primary should eventually write one entry.
    /\ WF_vars(\E i \in Server : LastTerm(log[i]) # globalCurrentTerm /\ ClientWrite(i))
    \*
    /\ WF_vars(AdvanceCommitPoint)
    /\ SF_vars(LearnCommitPointWithTermCheckAction)
    /\ SF_vars(LearnCommitPointFromSyncSourceNeverBeyondLastAppliedAction)

\* The specification must start with the initial state and transition according
\* to Next.
Spec == SpecBehavior /\ Liveness

\* Invariant for model-checking
NeverRollbackCommitted ==
    \A i \in Server: ~RollbackCommitted(i)

===============================================================================
