\* Copyright 2019 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

--------------------------------- MODULE RaftMongoReplTimestamp ---------------------------------
\* This is the formal specification for the Raft consensus algorithm in MongoDB.
\*
\* To run the model-checker, first edit the constants in MCRaftMongoReplTimestamp.cfg if desired, then:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh RaftMongoReplTimestamp

EXTENDS Integers, FiniteSets, Sequences, TLC

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

\* The server's lastDurable timestamp.
VARIABLE lastDurable

\* The server's lastApplied timestamp.
VARIABLE lastApplied

\* The server's committedSnapshot, including last and curr committedSnapshot for checking rollback.
VARIABLE committedSnapshot

oplogDurabilityVars == <<lastDurable, committedSnapshot>>
oplogVisibilityVars == <<lastApplied, commitPoint>>
oplogTimeVars == <<oplogDurabilityVars, oplogVisibilityVars>>
electionVars == <<currentTerm, state>>
serverVars == <<electionVars, oplogTimeVars>>

\* A Sequence of log entries. The index into this sequence is the index of the
\* log entry. Unfortunately, the Sequence module defines Head(s) as the entry
\* with index 1, so be careful not to use that!
VARIABLE log
logVars == <<committedEntries, log>>

\* The restart times for each server.
VARIABLE restartTimes

\* The failover times for each server.
VARIABLE failoverTimes
controlVars == <<restartTimes, failoverTimes>>
\* End of per server variables.
----

\* All variables; used for stuttering (asserting state hasn't changed).
vars == <<serverVars, logVars, controlVars>>

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

\* Return true if x is considered as a smaller or equal timestamp than y.
TimestampLTE(x, y) ==
    IF x.term < y.term THEN TRUE
    ELSE IF x.term > y.term THEN FALSE
    ELSE IF x.index <= y.index THEN TRUE
    ELSE FALSE

\* Return the min value from a set of timestamps in the form of {term, index}, or undefined if the set is empty.
MinTimestamp(s) == CHOOSE x \in s : \A y \in s : TimestampLTE(x, y)
MaxTimestamp(s) == CHOOSE x \in s : \A y \in s : TimestampLTE(y, x)

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
        /\ lastDurable[node].index >= logIndex
        /\ LogTerm(me, logIndex) = LogTerm(node, logIndex) }

\* Return whether Node i can learn the commit point from Node j.
CommitPointLessThan(i, j) ==
   \/ commitPoint[i].term < commitPoint[j].term
   \/ /\ commitPoint[i].term = commitPoint[j].term
      /\ commitPoint[i].index < commitPoint[j].index

\* Is it possible for node i's log to roll back based on j's log? If true, it
\* implies that i's log should remove entries to become a prefix of j's. This
\* is determined using top of oplog.
CanRollbackOplog(i, j) ==
    /\ Len(log[i]) > 0
    /\ \* The log with later term is more up-to-date
       LastTerm(log[i]) < LastTerm(log[j])
    /\
       \/ Len(log[i]) > Len(log[j])
       \/ /\ Len(log[i]) <= Len(log[j])
          /\ LastTerm(log[i]) /= LogTerm(j, Len(log[i]))

\* Update the last and curr for committedSnapshot[i].
UpdateCommittedSnapshot(i, newCommittedSnapshot) ==
    /\ committedSnapshot' = [committedSnapshot EXCEPT ![i] = [last |-> committedSnapshot[i].curr,
                                                              curr |-> newCommittedSnapshot]]

\* If lastApplied = lastDurable = committedSnapshot, restart won't help 
\* verify anything, so we optimize not doing restart at that time.                                                              
RestartIsUnncessary(i) ==
    /\ lastApplied[i].term = lastDurable[i].term
    /\ lastApplied[i].index = lastDurable[i].index
    /\ lastApplied[i].term = committedSnapshot[i].curr.term
    /\ lastApplied[i].index = committedSnapshot[i].curr.index

----
\* Define initial values for all variables

InitServerVars == /\ currentTerm       = [i \in Server |-> 0]
                  /\ restartTimes      = [i \in Server |-> 0]
                  /\ failoverTimes     = [i \in Server |-> 0]
                  /\ state             = [i \in Server |-> "Follower"]
                  /\ commitPoint       = [i \in Server |-> [term |-> 0, index |-> 0]]
                  /\ lastDurable       = [i \in Server |-> [term |-> 0, index |-> 0]]
                  /\ lastApplied       = [i \in Server |-> [term |-> 0, index |-> 0]]
                  /\ committedSnapshot   = [i \in Server |-> [last |-> [term |-> 0, index |-> 0],
                                                            curr |-> [term |-> 0, index |-> 0]]]
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
    /\ UNCHANGED <<committedEntries, serverVars, controlVars>>

\* Simulate journal flush, updating lastDurable to the top of log.
PersistOplog(i) == 
    /\ LET newLastDurable == [term |-> LastTerm(log[i]), index |-> Len(log[i])]
        IN /\ ~TimestampLTE(newLastDurable, lastDurable[i])
           /\ lastDurable' = [lastDurable EXCEPT ![i] = newLastDurable]
    /\ UNCHANGED <<oplogVisibilityVars, electionVars, logVars, controlVars, committedSnapshot>>

\* Simulate oplog applier, updating lastApplied to the top of log.
ApplyOplog(i) == 
    /\ state[i] = "Follower"
    /\ LET newLastApplied == [term |-> LastTerm(log[i]), index |-> Len(log[i])]
           newCommittedSnapshot == MinTimestamp({commitPoint[i], newLastApplied})
        IN /\ ~TimestampLTE(newLastApplied, lastApplied[i])
           /\ lastApplied' = [lastApplied EXCEPT ![i] = newLastApplied]
           /\ UpdateCommittedSnapshot(i, newCommittedSnapshot)
    /\ UNCHANGED <<lastDurable, commitPoint, electionVars, logVars, controlVars>>

\* Node i learns the commit point from j via heartbeat.
LearnCommitPoint(i, j) ==
    /\ CommitPointLessThan(i, j)
    /\ LET newCommitPoint == commitPoint[j]
           newCommittedSnapshot == MinTimestamp({newCommitPoint, lastApplied[i]})
        IN /\ commitPoint' = [commitPoint EXCEPT ![i] = commitPoint[j]]
           /\ UpdateCommittedSnapshot(i, newCommittedSnapshot)
    /\ UNCHANGED <<lastDurable, lastApplied, electionVars, logVars, controlVars>>

RollbackOplog(i, j) ==
    /\ CanRollbackOplog(i, j)
    \* Rollback 1 oplog entry
    /\ LET newLog == [index2 \in 1..(Len(log[i]) - 1) |-> log[i][index2]]
           newOplogWritten == [term |-> LastTerm(newLog), index |-> Len(newLog)]
           newLastDurable == MinTimestamp({lastDurable[i], newOplogWritten})
           newLastApplied == MinTimestamp({lastApplied[i], newOplogWritten})
           newCommittedSnapshot == MinTimestamp({commitPoint[i], newLastApplied})
        IN /\ log' = [log EXCEPT ![i] = newLog]
           /\ lastApplied' = [lastApplied EXCEPT ![i] = newLastApplied]
           /\ lastDurable' = [lastDurable EXCEPT ![i] = newLastDurable]
           /\ UpdateCommittedSnapshot(i, newCommittedSnapshot)
    /\ UNCHANGED <<electionVars, committedEntries, commitPoint, controlVars>>

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
    /\ UNCHANGED <<committedEntries, oplogTimeVars, logVars, controlVars>>
    
\* Restart node i, throwing every log that is not durable and setting lastApplied to 
\* stableTimestamp, which is assumed to be the same as committedSnapshot in this spec.
Restart(i) ==
    /\ state' = [state EXCEPT ![i] = "Follwer"]
    /\ ~RestartIsUnncessary(i)
    /\ restartTimes' = [restartTimes EXCEPT ![i] = restartTimes[i] + 1]
    /\ LET newLastDurable == MaxTimestamp({lastDurable[i], committedSnapshot[i].curr})
           newLog == [index2 \in 1..(newLastDurable.index) |-> log[i][index2]]
           newLastApplied == committedSnapshot[i].curr
       IN /\ log' = [log EXCEPT ![i] = newLog]
          /\ lastApplied' = [lastApplied EXCEPT ![i] = newLastApplied]
          /\ lastDurable' = [lastDurable EXCEPT ![i] = newLastDurable]
    /\ UNCHANGED <<committedSnapshot, commitPoint, currentTerm, committedEntries, failoverTimes>>
    
\* ACTION
\* Node i is leader and steps down for any reason.
Stepdown(i) ==
    /\ state[i] = "Leader"
    /\ state' = [state EXCEPT ![i] = "Follower"]
    /\ failoverTimes' = [failoverTimes EXCEPT ![i] = failoverTimes[i] + 1]
    /\ UNCHANGED <<committedEntries, currentTerm, logVars, oplogTimeVars, restartTimes>>

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
        IN  /\ log' = [log EXCEPT ![i] = newLog]
            /\ lastApplied' = [lastApplied EXCEPT ![i] = [term |-> LastTerm(newLog), index |-> Len(newLog)]]
    /\ UNCHANGED <<committedEntries, electionVars, commitPoint, oplogDurabilityVars, controlVars>>

UpdateTermThroughHeartbeat(i, j) ==
    /\ currentTerm[j] > currentTerm[i]
    /\ currentTerm' = [currentTerm EXCEPT ![i] = currentTerm[j]]
    /\ state' = [state EXCEPT ![i] = "Follower"]
    /\ UNCHANGED <<oplogTimeVars, logVars, controlVars>>

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
               newCommittedSnapshot == MinTimestamp({newCommitPoint, lastApplied[leader]})
            IN /\ commitPoint' = [commitPoint EXCEPT ![leader] = newCommitPoint]
               /\ UpdateCommittedSnapshot(leader, newCommittedSnapshot)
        /\ committedEntries' = committedEntries \union {[
               term |-> LogTerm(leader, i),
               index |-> i
           ] : i \in commitPoint[leader].index + 1..committedIndex}
    /\ UNCHANGED <<electionVars, log, lastApplied, lastDurable, controlVars>>

\* ACTION
\* Node i learns the commit point from j via heartbeat with term check
LearnCommitPointWithTermCheck(i, j) ==
    /\ LastTerm(log[i]) = commitPoint[j].term
    /\ LearnCommitPoint(i, j)

\* ACTION
\* Node i learns the commit point from j while tailing j's oplog
LearnCommitPointFromSyncSourceNeverBeyondTopOfOplog(i, j) ==
    \* j is a potential sync source, either ahead of or equal to i's oplog
    /\ \/ CanSyncFrom(i, j)
       \/ log[i] = log[j]
    /\ CommitPointLessThan(i, j)
    \* Never beyond top of oplog
    /\ LET myCommitPoint ==
            \* If j's term is less than or equal to i's, commit point can be ahead.
            IF commitPoint[j].term <= LastTerm(log[i])
            THEN commitPoint[j]
            ELSE [term |-> LastTerm(log[i]), index |-> Len(log[i])]
           newCommittedSnapshot == MinTimestamp({myCommitPoint, lastApplied[i]})
       IN /\ commitPoint' = [commitPoint EXCEPT ![i] = myCommitPoint]
          /\ UpdateCommittedSnapshot(i, newCommittedSnapshot)
    /\ UNCHANGED <<committedEntries, electionVars, logVars, lastApplied, lastDurable, controlVars>>

----
AppendOplogAction ==
    \E i,j \in Server : AppendOplog(i, j)

RollbackOplogAction ==
    \E i,j \in Server : RollbackOplog(i, j)

BecomePrimaryByMagicAction ==
    \E i \in Server : \E ayeVoters \in SUBSET(Server) : BecomePrimaryByMagic(i, ayeVoters)

RestartAction ==
    \E i \in Server: Restart(i)

StepdownAction ==
    \E i \in Server : Stepdown(i)

ClientWriteAction ==
    \E i \in Server : ClientWrite(i)

PersistOplogAction ==
    \E i \in Server : PersistOplog(i)

ApplyOplogAction ==
    \E i \in Server : ApplyOplog(i)

UpdateTermThroughHeartbeatAction ==
    \E i, j \in Server : UpdateTermThroughHeartbeat(i, j)

LearnCommitPointWithTermCheckAction ==
    \E i, j \in Server : LearnCommitPointWithTermCheck(i, j)

LearnCommitPointFromSyncSourceNeverBeyondTopOfOplogAction ==
    \E i, j \in Server : LearnCommitPointFromSyncSourceNeverBeyondTopOfOplog(i, j)

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
    \/ RestartAction
    \/ StepdownAction
    \/ ClientWriteAction
    \/ PersistOplogAction
    \/ ApplyOplogAction
    \/ UpdateTermThroughHeartbeatAction
    \*
    \* --- Commit point learning protocol
    \/ AdvanceCommitPoint
    \/ LearnCommitPointWithTermCheckAction
    \/ LearnCommitPointFromSyncSourceNeverBeyondTopOfOplogAction

SpecBehavior == Init /\ [][Next]_vars

Liveness ==
    /\ SF_vars(AppendOplogAction)
    /\ SF_vars(RollbackOplogAction)
    /\ SF_vars(PersistOplogAction)
    /\ SF_vars(ApplyOplogAction)
    \* A new primary should eventually write one entry.
    /\ WF_vars(\E i \in Server : LastTerm(log[i]) # GlobalCurrentTerm /\ ClientWrite(i))
    \*
    /\ WF_vars(AdvanceCommitPoint)
    /\ SF_vars(LearnCommitPointWithTermCheckAction)
    /\ SF_vars(LearnCommitPointFromSyncSourceNeverBeyondTopOfOplogAction)

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

CommittedSnapshotNeverRollback == 
    \A i \in Server: TimestampLTE(committedSnapshot[i].last, committedSnapshot[i].curr)

\* Liveness check

\* This isn't accurate for any infinite behavior specified by Spec, but it's fine
\* for any finite behavior with the liveness we can check with the model checker.
\* This is to check at any time, if two nodes' commit points are not the same, they
\* will be the same eventually.
\* This is checked after all possible rollback is done.
\* This property check is costly so we disable it by default. To activate it,
\* uncomment the following lines and the property in MCRaftMongoReplTimestamp.cfg
\* CommitPointEventuallyPropagates ==
\*     /\ \A i, j \in Server:
\*         [](commitPoint[i] # commitPoint[j] ~>
\*                <>(~ENABLED RollbackOplogAction => commitPoint[i] = commitPoint[j]))

===============================================================================
