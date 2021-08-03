\* Copyright 2019 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

--------------------------------- MODULE RaftMongoWithRaftReconfig --------------------------------
\* This is the formal specification for the Raft consensus algorithm in MongoDB.
\* It allows reconfig using the protocol for single server membership changes described in Raft.
\* Note that we did not choose to implement the protocol for single server membership changes
\* described in Raft. This specification was for exploratory purposes only.

EXTENDS Naturals, FiniteSets, Sequences, TLC

\* The set of server IDs
CONSTANTS Server

\* Server states.
\* Candidate is not used, but this is fine.
CONSTANTS Follower, Candidate, Leader

\* A reserved value.
CONSTANTS Nil

----
\* Global variables

\* Servers in a given config version.
\* e.g. << {S1, S2}, {S1, S2, S3} >>
VARIABLE configs

\* The set of log entries that have been acknowledged as committed, i.e.
\* "immediately committed" entries. It does not include "prefix committed"
\* entries, which are allowed to roll back on minority nodes.
VARIABLE committedEntries

----
\* The following variables are all per server (functions with domain Server).

\* The server's term number.
VARIABLE currentTerm

\* The server's state (Follower, Candidate, or Leader).
VARIABLE state

serverVars == <<currentTerm, state>>

\* A Sequence of log entries. The index into this sequence is the index of the
\* log entry. Unfortunately, the Sequence module defines Head(s) as the entry
\* with index 1, so be careful not to use that!
VARIABLE log
logVars == <<log, committedEntries>>

\* End of per server variables.
----

\* All variables; used for stuttering (asserting state hasn't changed).
vars == <<serverVars, logVars, configs>>

----
\* Helpers

\* The term of the last entry in a log, or 0 if the log is empty.
GetTerm(xlog, index) == IF index = 0 THEN 0 ELSE xlog[index].term
LogTerm(i, index) == GetTerm(log[i], index)
LastTerm(xlog) == GetTerm(xlog, Len(xlog))

\* Return the minimum value from a set, or undefined if the set is empty.
Min(s) == CHOOSE x \in s : \A y \in s : x <= y
\* Return the maximum value from a set, or undefined if the set is empty.
Max(s) == CHOOSE x \in s : \A y \in s : x >= y

\* The config version in the node's last entry.
GetConfigVersion(i) == log[i][Len(log[i])].configVersion

\* Gets the node's first entry with a given config version.
GetConfigEntry(i, configVersion) == LET configEntries == {index \in 1..Len(log[i]) : 
                                                            log[i][index].configVersion = configVersion}
                                    IN Min(configEntries)

\* The servers that are in the same config as i.
ServerViewOn(i) == configs[GetConfigVersion(i)]

\* The set of all quorums. This just calculates simple majorities, but the only
\* important property is that every quorum overlaps with every other.
Quorum(me) == {sub \in SUBSET(ServerViewOn(me)) : Cardinality(sub) * 2 > Cardinality(ServerViewOn(me))}

----
\* Define initial values for all variables
InitServerVars == /\ currentTerm = [i \in Server |-> 0]
                  /\ state       = [i \in Server |-> Follower]
InitLogVars == /\ log              = [i \in Server |-> << [term |-> 0, configVersion |-> 1] >>]
               /\ committedEntries = {[term |-> 0, index |-> 1]}
InitConfigs == configs = << Server >>
Init == /\ InitServerVars
        /\ InitLogVars
        /\ InitConfigs

----
\* Message handlers
\* i = recipient, j = sender, m = message

AppendOplog(i, j) ==
    /\ state[i] = Follower  \* Disable primary catchup and draining
    /\ j \in ServerViewOn(i)  \* j is in the config of i.
    /\ Len(log[i]) < Len(log[j])
    /\ LastTerm(log[i]) = LogTerm(j, Len(log[i]))
    /\ log' = [log EXCEPT ![i] = Append(log[i], log[j][Len(log[i]) + 1])]
    /\ UNCHANGED <<serverVars, committedEntries, configs>>

CanRollbackOplog(i, j) ==
    /\ j \in ServerViewOn(i)  \* j is in the config of i.
    /\ Len(log[i]) > 0
    /\ \* The log with later term is more up-to-date
       LastTerm(log[i]) < LastTerm(log[j])
    /\
       \/ Len(log[i]) > Len(log[j])
       \* There seems no short-cut of OR clauses, so I have to specify the negative case
       \/ /\ Len(log[i]) <= Len(log[j])
          /\ LastTerm(log[i]) /= LogTerm(j, Len(log[i]))

RollbackOplog(i, j) ==
    /\ CanRollbackOplog(i, j)
    \* Rollback 1 oplog entry
    /\ LET new == [index2 \in 1..(Len(log[i]) - 1) |-> log[i][index2]]
         IN log' = [log EXCEPT ![i] = new]
    /\ UNCHANGED <<serverVars, committedEntries, configs>>

\* The set of nodes in my config that has log[me][logIndex] in their oplog
Agree(me, logIndex) ==
    { node \in ServerViewOn(me) :
        /\ Len(log[node]) >= logIndex
        /\ LogTerm(me, logIndex) = LogTerm(node, logIndex) }

NotBehind(me, j) == \/ LastTerm(log[me]) > LastTerm(log[j])
                    \/ /\ LastTerm(log[me]) = LastTerm(log[j])
                       /\ Len(log[me]) >= Len(log[j])

\* ACTION
\* i = the new primary node.
BecomePrimaryByMagic(i, ayeVoters) ==
    /\ \A j \in ayeVoters : /\ i \in ServerViewOn(j)
                            /\ NotBehind(i, j)
                            /\ currentTerm[j] <= currentTerm[i]
    /\ ayeVoters \in Quorum(i)
    /\ state' = [index \in Server |-> IF index \notin ayeVoters
                                      THEN state[index]
                                      ELSE IF index = i THEN Leader ELSE Follower]
    /\ currentTerm' = [index \in Server |-> IF index \in (ayeVoters \union {i})
                                            THEN currentTerm[i] + 1 
                                            ELSE currentTerm[index]]
    /\ UNCHANGED <<logVars, configs>>

\* ACTION
\* Leader i receives a client request to add v to the log.
ClientWrite(i) ==
    /\ state[i] = Leader
    /\ LET entry == [term  |-> currentTerm[i], configVersion |-> GetConfigVersion(i)]
           newLog == Append(log[i], entry)
       IN  log' = [log EXCEPT ![i] = newLog]
    /\ UNCHANGED <<serverVars, committedEntries, configs>>
        
\* ACTION
\* Commit the latest log entry on a primary.
AdvanceCommitPoint ==
    \E leader \in Server : \E acknowledgers \in SUBSET Server :
        /\ state[leader] = Leader
        /\ acknowledgers \subseteq Agree(leader, Len(log[leader]))
        /\ acknowledgers \in Quorum(leader)
        \* If we comment out the following line, a replicated log entry from old primary will voilate the safety.
        \* [ P (2), S (), S ()]
        \* [ S (2), S (), P (3)]
        \* [ S (2), S (2), P (3)] !!! the log from term 2 shouldn't be considered as committed.
        /\ LogTerm(leader, Len(log[leader])) = currentTerm[leader]
        \* If an acknowledger has a higher term, the leader would step down.
        /\ \A j \in acknowledgers : currentTerm[j] <= currentTerm[leader]
        /\ committedEntries' = committedEntries \union {[term |-> LastTerm(log[leader]), index |-> Len(log[leader])]}
        /\ UNCHANGED <<serverVars, log, configs>>
       
UpdateTermThroughHeartbeat(i, j) ==
    /\ j \in ServerViewOn(i)  \* j is in the config of i.
    /\ currentTerm[j] > currentTerm[i]
    /\ currentTerm' = [currentTerm EXCEPT ![i] = currentTerm[j]]
    /\ state' = [state EXCEPT ![i] = IF ~(state[i] = Leader) THEN state[i] ELSE Follower]
    /\ UNCHANGED <<logVars, configs>>
        
Reconfig(i, newConfig) ==
    /\ state[i] = Leader
    /\ i \in newConfig
    \* Only support single node addition/removal.
    /\ Cardinality(ServerViewOn(i) \ newConfig) + Cardinality(newConfig \ ServerViewOn(i)) <= 1
    \* The config entry must be committed.
    /\ LET configEntry == GetConfigEntry(i, GetConfigVersion(i))
       IN [term |-> log[i][configEntry].term, index |-> configEntry] \in committedEntries
    \* The primary must have committed an entry in its current term.
    /\ \E entry \in committedEntries : entry.term = currentTerm[i]
    /\ configs' = Append(configs, newConfig)
    /\ LET entry == [term  |-> currentTerm[i], configVersion |-> Len(configs) + 1]
           newLog == Append(log[i], entry)
       IN  log' = [log EXCEPT ![i] = newLog]
    /\ UNCHANGED <<serverVars, committedEntries>>

----
AppendOplogAction ==
    \E i,j \in Server : AppendOplog(i, j)

RollbackOplogAction ==
    \E i,j \in Server : RollbackOplog(i, j)

BecomePrimaryByMagicAction ==
    \E i \in Server : \E ayeVoters \in SUBSET(Server) : BecomePrimaryByMagic(i, ayeVoters)

ClientWriteAction ==
    \E i \in Server : ClientWrite(i)
    
UpdateTermThroughHeartbeatAction ==
    \E i,j \in Server : UpdateTermThroughHeartbeat(i, j)
    
ReconfigAction ==
    \E i \in Server : \E newConfig \in SUBSET(Server) : Reconfig(i, newConfig)

----
\* Defines how the variables may transition.
Next ==
    \* --- Replication protocol
    \/ AppendOplogAction
    \/ RollbackOplogAction
    \/ BecomePrimaryByMagicAction
    \/ ClientWriteAction
    \/ AdvanceCommitPoint
    \/ ReconfigAction
    \/ UpdateTermThroughHeartbeatAction

Liveness ==
    /\ SF_vars(AppendOplogAction)
    /\ SF_vars(RollbackOplogAction)
    \* A new primary should eventually write one entry.
    /\ WF_vars(\E i \in Server : LastTerm(log[i]) # currentTerm[i] /\ ClientWrite(i))
    \* /\ WF_vars(ClientWriteAction)

\* The specification must start with the initial state and transition according
\* to Next.
Spec == Init /\ [][Next]_vars /\ Liveness

\* RollbackCommitted and NeverRollbackCommitted are not actions.
\* They are used for verification.
RollbackCommitted(i) ==
    /\ [term |-> LastTerm(log[i]), index |-> Len(log[i])] \in committedEntries
    /\ \E j \in Server: CanRollbackOplog(i, j)

NeverRollbackCommitted ==
    \A i \in Server: ~RollbackCommitted(i)
    
TwoPrimariesInSameTerm == 
    \E i, j \in Server :
        /\ i # j 
        /\ currentTerm[i] = currentTerm[j] 
        /\ state[i] = Leader 
        /\ state[j] = Leader

NoTwoPrimariesInSameTerm == ~TwoPrimariesInSameTerm

===============================================================================
