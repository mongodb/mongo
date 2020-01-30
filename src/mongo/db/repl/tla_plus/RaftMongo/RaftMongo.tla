\* Copyright 2019 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

--------------------------------- MODULE RaftMongo ---------------------------------
\* This is the formal specification for the Raft consensus algorithm in MongoDB.
\*
\* To run the model-checker, first edit the constants in MCRaftMongo.cfg if desired, then:
\*     cd src/mongo/db/repl/tla_plus
\*     ./model-check.sh RaftMongo

EXTENDS Integers, FiniteSets, Sequences

\* The set of server IDs.
CONSTANT Server

\* The maximum number of oplog entries that can be created on the primary in one
\* action. For model-checking, this can be 1 or a small number.
CONSTANT MaxClientWriteSize

\* The set of log entries that have been acknowledged as committed, both "immediately committed" and
\* "prefix committed".
VARIABLE committedEntries

----
\* The following variables are all per server (functions with domain Server).

\* The server's term number.
VARIABLE currentTerm

\* The server's state ("Follower" or "Leader").
VARIABLE state

\* The commit point learned by each server.
VARIABLE commitPoint

electionVars == <<currentTerm, state>>
serverVars == <<electionVars, commitPoint>>

\* A Sequence of log entries. The index into this sequence is the index of the
\* log entry. Unfortunately, the Sequence module defines Head(s) as the entry
\* with index 1, so be careful not to use that!
VARIABLE log
logVars == <<committedEntries, log>>

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
Leaders == {s \in Server : state[s] = "Leader"}
Range(f) == {f[x] : x \in DOMAIN f}

\* Return the maximum value from a set, or undefined if the set is empty.
Max(s) == CHOOSE x \in s : \A y \in s : x >= y

GlobalCurrentTerm == Max(Range(currentTerm))

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

\* Is it possible for node i's log to roll back based on j's log? If true, it
\* implies that i's log should remove entries to become a prefix of j's.
CanRollbackOplog(i, j) ==
    /\ Len(log[i]) > 0
    /\ \* The log with later term is more up-to-date
       LastTerm(log[i]) < LastTerm(log[j])
    /\
       \/ Len(log[i]) > Len(log[j])
       \/ /\ Len(log[i]) <= Len(log[j])
          /\ LastTerm(log[i]) /= LogTerm(j, Len(log[i]))

----
\* Define initial values for all variables

InitServerVars == /\ currentTerm       = [i \in Server |-> 0]
                  /\ state             = [i \in Server |-> "Follower"]
                  /\ commitPoint       = [i \in Server |-> [term |-> 0, index |-> 0]]
InitLogVars ==    /\ log               = [i \in Server |-> << >>]
                  /\ committedEntries  = {}
Init == /\ InitServerVars
        /\ InitLogVars

----
\* Message handlers
\* i = recipient, j = sender

\* Receive one or more oplog entries from j.
AppendOplog(i, j) ==
    /\ CanSyncFrom(i, j)
    /\ state[i] = "Follower"
    /\ \E lastAppended \in (Len(log[i]) + 1)..Len(log[j]):
        LET appendedEntries == SubSeq(log[j], Len(log[i]) + 1, lastAppended)
         IN log' = [log EXCEPT ![i] = log[i] \o appendedEntries]
    /\ UNCHANGED <<committedEntries, serverVars>>

\* Node i learns the commit point from j via heartbeat.
LearnCommitPoint(i, j) ==
    /\ CommitPointLessThan(i, j)
    /\ commitPoint' = [commitPoint EXCEPT ![i] = commitPoint[j]]
    /\ UNCHANGED <<committedEntries, electionVars, logVars>>

RollbackOplog(i, j) ==
    /\ CanRollbackOplog(i, j)
    \* Rollback 1 oplog entry
    /\ LET new == [index2 \in 1..(Len(log[i]) - 1) |-> log[i][index2]]
        IN log' = [log EXCEPT ![i] = new]
    /\ UNCHANGED <<serverVars, committedEntries>>

\* ACTION
\* Node i is elected by a majority, and nodes that voted for it can't still be primary.
\* A stale primary might persist among the minority that didn't vote for it.
BecomePrimaryByMagic(i, ayeVoters) ==
    /\ \A j \in ayeVoters : /\ NotBehind(i, j)
                            /\ currentTerm[j] <= currentTerm[i]
    /\ IsMajority(ayeVoters)
    /\ state' = [index \in Server |-> IF index \notin ayeVoters
                                      THEN state[index]
                                      ELSE IF index = i THEN "Leader" ELSE "Follower"]
    /\ currentTerm' = [index \in Server |-> IF index \in (ayeVoters \union {i})
                                            THEN currentTerm[i] + 1
                                            ELSE currentTerm[index]]
    /\ UNCHANGED <<committedEntries, commitPoint, logVars>>
    
    
\* ACTION
\* Node i is leader and steps down for any reason.
Stepdown(i) ==
    /\ state[i] = "Leader"
    /\ state' = [state EXCEPT ![i] = "Follower"]
    /\ UNCHANGED <<committedEntries, currentTerm, commitPoint, logVars>>

\* ACTION
\* Leader i receives a client request to add one or more entries to the log.
\* There can be multiple leaders, each in a different term. A leader writes
\* an oplog entry in its own term.
ClientWrite(i) ==
    /\ state[i] = "Leader"
    /\ \E numEntries \in 1..MaxClientWriteSize :
        LET entry == [term |-> currentTerm[i]]
            newEntries == [ j \in 1..numEntries |-> entry ]
            newLog == log[i] \o newEntries
        IN  log' = [log EXCEPT ![i] = newLog]
    /\ UNCHANGED <<committedEntries, serverVars>>

UpdateTermThroughHeartbeat(i, j) ==
    /\ currentTerm[j] > currentTerm[i]
    /\ currentTerm' = [currentTerm EXCEPT ![i] = currentTerm[j]]
    /\ state' = [state EXCEPT ![i] = "Follower"]
    /\ UNCHANGED <<commitPoint, logVars>>

\* ACTION
AdvanceCommitPoint ==
    \E leader \in Leaders :
    \E acknowledgers \in SUBSET Server :
    \* New commitPoint is any committed log index after current commitPoint
    \E committedIndex \in (commitPoint[leader].index + 1)..Len(log[leader]) :
        /\ IsMajority(acknowledgers)
        /\ acknowledgers \subseteq Agree(leader, committedIndex)
        \* New commitPoint is an entry written by this leader.
        /\ LogTerm(leader, committedIndex) = currentTerm[leader]
        \* If an acknowledger has a higher term, the leader would step down.
        /\ \A j \in acknowledgers : currentTerm[j] <= currentTerm[leader]
        /\ LET newCommitPoint == [
                   term |-> LogTerm(leader, committedIndex),
                   index |-> committedIndex
               ]
            IN /\ commitPoint' = [commitPoint EXCEPT ![leader] = newCommitPoint]
        /\ committedEntries' = committedEntries \union {[
               term |-> LogTerm(leader, i),
               index |-> i
           ] : i \in commitPoint[leader].index + 1..committedIndex}
    /\ UNCHANGED <<electionVars, log>>

\* ACTION
\* Node i learns the commit point from j via heartbeat with term check
LearnCommitPointWithTermCheck(i, j) ==
    /\ LastTerm(log[i]) = commitPoint[j].term
    /\ LearnCommitPoint(i, j)

\* ACTION
\* Node i learns the commit point from j while tailing j's oplog
LearnCommitPointFromSyncSourceNeverBeyondLastApplied(i, j) ==
    \* j is a potential sync source, either ahead of or equal to i's oplog
    /\ \/ CanSyncFrom(i, j)
       \/ log[i] = log[j]
    /\ CommitPointLessThan(i, j)
    \* Never beyond last applied
    /\ LET myCommitPoint ==
            \* If j's term is less than or equal to i's, commit point can be ahead.
            IF commitPoint[j].term <= LastTerm(log[i])
            THEN commitPoint[j]
            ELSE [term |-> LastTerm(log[i]), index |-> Len(log[i])]
       IN commitPoint' = [commitPoint EXCEPT ![i] = myCommitPoint]
    /\ UNCHANGED <<committedEntries, electionVars, logVars>>

----
AppendOplogAction ==
    \E i,j \in Server : AppendOplog(i, j)

RollbackOplogAction ==
    \E i,j \in Server : RollbackOplog(i, j)

BecomePrimaryByMagicAction ==
    \E i \in Server : \E ayeVoters \in SUBSET(Server) : BecomePrimaryByMagic(i, ayeVoters)

StepdownAction ==
    \E i \in Server : Stepdown(i)

ClientWriteAction ==
    \E i \in Server : ClientWrite(i)

UpdateTermThroughHeartbeatAction ==
    \E i, j \in Server : UpdateTermThroughHeartbeat(i, j)

LearnCommitPointWithTermCheckAction ==
    \E i, j \in Server : LearnCommitPointWithTermCheck(i, j)

LearnCommitPointFromSyncSourceNeverBeyondLastAppliedAction ==
    \E i, j \in Server : LearnCommitPointFromSyncSourceNeverBeyondLastApplied(i, j)

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
    \/ StepdownAction
    \/ ClientWriteAction
    \/ UpdateTermThroughHeartbeatAction
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
    /\ WF_vars(\E i \in Server : LastTerm(log[i]) # GlobalCurrentTerm /\ ClientWrite(i))
    \*
    /\ WF_vars(AdvanceCommitPoint)
    /\ SF_vars(LearnCommitPointWithTermCheckAction)
    /\ SF_vars(LearnCommitPointFromSyncSourceNeverBeyondLastAppliedAction)

\* The specification must start with the initial state and transition according
\* to Next.
Spec == SpecBehavior /\ Liveness

----

\* Properties to check

TwoPrimariesInSameTerm ==
    \E i, j \in Server :
        /\ i # j
        /\ currentTerm[i] = currentTerm[j]
        /\ state[i] = "Leader"
        /\ state[j] = "Leader"

NoTwoPrimariesInSameTerm == ~TwoPrimariesInSameTerm

RollbackCommitted(i) ==
    /\ [term |-> LastTerm(log[i]), index |-> Len(log[i])] \in committedEntries
    /\ \E j \in Server: CanRollbackOplog(i, j)

NeverRollbackCommitted ==
    \A i \in Server: ~RollbackCommitted(i)

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

===============================================================================
