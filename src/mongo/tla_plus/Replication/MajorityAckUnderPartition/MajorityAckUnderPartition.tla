\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------ MODULE MajorityAckUnderPartition ------------------
\* Formal specification for SERVER-101041.
\*
\* "Updates may wrongly get majority acknowledged if primary gets
\*  isolated due to network partition."
\*
\* Scenario from the ticket:
\*   - 3-node replica set, N0 primary, N1/N2 secondary.
\*   - Client inserts {x: 10} w:majority on N0 (replicated everywhere).
\*   - Network partitions N0 from {N1, N2}. N1 wins a new election in a
\*     higher term and becomes primary; N0 still believes it is primary
\*     (no-op writes / heartbeats have not yet informed it).
\*   - Client updates {x: 20} w:majority on N1 (ack'd by N1, N2).
\*   - Client updates {x: 10} w:majority on N0. N0 sees its in-memory
\*     state is already {x: 10}, the update is a no-op, so it short-
\*     circuits and returns majority-ack WITHOUT replicating.
\*   - Linearizable read from the new majority returns {x: 20}, which
\*     contradicts the client's just-acknowledged "successful" write of
\*     {x: 10}. The acknowledgement is a lie.
\*
\* The safety invariant we want: any acknowledgement labelled
\* "majority" must be backed by a real majority quorum that intersects
\* every future leader's quorum (i.e. the canonical Raft / w:majority
\* contract).
\*
\* This spec deliberately models the no-op short-circuit as a separate
\* action (NoOpMajorityAckOnStalePrimary) so that the bug is visible as
\* a single arrow in the TLC counterexample trace.
\*
\* To run the model-checker:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Replication/MajorityAckUnderPartition

EXTENDS Integers, FiniteSets, Sequences, TLC

\* The set of server IDs.
CONSTANT Server

\* Maximum number of distinct (key, value) writes a client may issue.
CONSTANT MaxWrites

\* The set of keys a client can write. One key is enough to trigger the
\* bug; we keep it parameterised for sanity.
CONSTANT Key

\* The set of values a client can write per key. Must include at least
\* two distinct values so the no-op short-circuit can fire.
CONSTANT Value

\* ---- Per-server variables ----

\* Each server's belief about its own role.
VARIABLE state            \* [Server -> {"Primary", "Secondary"}]

\* Each server's current term number.
VARIABLE currentTerm      \* [Server -> Nat]

\* Each server's local oplog: a sequence of [term, key, value] entries.
VARIABLE log              \* [Server -> Seq([term, key, value])]

\* Each server's local view of the user-visible state, keyed by key.
\* This mirrors mongod's in-memory document; it's what a "no-op write
\* short-circuit" would inspect before deciding to skip a replication.
VARIABLE store            \* [Server -> [Key -> Value \cup {NoValue}]]

\* ---- Global variables ----

\* The set of network links that are CURRENTLY usable. A link is an
\* unordered pair {i, j}. If {i, j} \notin network, then i and j cannot
\* exchange heartbeats / oplog / votes.
VARIABLE network          \* SUBSET (SUBSET Server)

\* The set of acknowledgements ever returned to clients with
\* writeConcern majority. Each entry is [key |-> k, value |-> v,
\* term |-> t, server |-> s]. We never delete from this set; once a
\* client is told "majority committed", that promise is permanent.
VARIABLE majorityAcks     \* SUBSET [key, value, term, server]

\* Count of writes issued so far (used as a TLC state-space cap).
VARIABLE writeCount       \* Nat

vars == <<state, currentTerm, log, store, network, majorityAcks,
          writeCount>>

\* A sentinel value for "key has never been written". The config must
\* not list this string in the Value set.
NoValue == "NoValue"

----
\* Helpers

IsMajority(S) == Cardinality(S) * 2 > Cardinality(Server)

\* The set of servers reachable from i via a single network link.
\* A server is always reachable from itself (loopback).
Reachable(i) ==
    {i} \cup {j \in Server : j # i /\ {i, j} \in network}

\* Term of the last log entry (0 if empty).
LastTerm(s) ==
    IF Len(log[s]) = 0 THEN 0 ELSE log[s][Len(log[s])].term

\* "s1 is at least as up-to-date as s2" in the Raft sense.
UpToDate(s1, s2) ==
    \/ LastTerm(s1) > LastTerm(s2)
    \/ /\ LastTerm(s1) = LastTerm(s2)
       /\ Len(log[s1]) >= Len(log[s2])

\* Current value of key k on server s (or NoValue if never written).
CurrentValue(s, k) == store[s][k]

\* Apply an oplog entry to a per-server store.
ApplyEntry(st, entry) ==
    [st EXCEPT ![entry.key] = entry.value]

\* Replay a whole log onto an empty store. Used after rollback.
ReplayLog(xlog) ==
    LET InitStore == [k \in Key |-> NoValue]
        F[i \in 0..Len(xlog)] ==
            IF i = 0 THEN InitStore
            ELSE ApplyEntry(F[i - 1], xlog[i])
    IN  F[Len(xlog)]

----
\* Initial state

Init ==
    /\ state         = [s \in Server |-> "Secondary"]
    /\ currentTerm   = [s \in Server |-> 0]
    /\ log           = [s \in Server |-> << >>]
    /\ store         = [s \in Server |-> [k \in Key |-> NoValue]]
    /\ network       = { {i, j} : i, j \in Server } \ { {s} : s \in Server }
    /\ majorityAcks  = {}
    /\ writeCount    = 0

----
\* Actions

\* A node wins an election from a majority of nodes reachable from it.
\* This abstracts the full Raft election protocol; we just require that
\* the voters are reachable, up-to-date, and form a majority.
Elect(i) ==
    /\ \E voters \in SUBSET Reachable(i) :
        /\ i \in voters
        /\ IsMajority(voters)
        /\ \A v \in voters : UpToDate(i, v)
        /\ \A v \in voters : currentTerm[v] < currentTerm[i] + 1
        /\ state' = [s \in Server |->
                        IF s = i THEN "Primary"
                        ELSE IF s \in voters THEN "Secondary"
                        ELSE state[s]]
        /\ currentTerm' = [s \in Server |->
                              IF s \in voters
                              THEN currentTerm[i] + 1
                              ELSE currentTerm[s]]
    /\ UNCHANGED <<log, store, network, majorityAcks, writeCount>>

\* A primary accepts a client write and appends to its own log. The
\* write is NOT yet majority-committed.
ClientWrite(i, k, v) ==
    /\ state[i] = "Primary"
    /\ writeCount < MaxWrites
    /\ LET entry == [term |-> currentTerm[i], key |-> k, value |-> v]
       IN  /\ log'   = [log   EXCEPT ![i] = Append(log[i], entry)]
           /\ store' = [store EXCEPT ![i] = ApplyEntry(store[i], entry)]
    /\ writeCount' = writeCount + 1
    /\ UNCHANGED <<state, currentTerm, network, majorityAcks>>

\* THE BUG (SERVER-101041).
\*
\* Primary i receives a client request to set k := v. Its in-memory
\* store already has k = v. mongod's no-op short-circuit returns
\* "majority committed" without producing an oplog entry or contacting
\* any secondary. The acknowledgement is recorded GLOBALLY (i.e. is
\* visible to the client / linearizable readers) without any quorum
\* having observed it.
\*
\* This is the action we expect to violate NoSplitBrainAck.
NoOpMajorityAckOnStalePrimary(i, k, v) ==
    /\ state[i] = "Primary"
    /\ writeCount < MaxWrites
    /\ CurrentValue(i, k) = v
    /\ majorityAcks' = majorityAcks \cup
                       {[key |-> k, value |-> v,
                         term |-> currentTerm[i], server |-> i]}
    /\ writeCount' = writeCount + 1
    /\ UNCHANGED <<state, currentTerm, log, store, network>>

\* A secondary j fetches one new log entry from primary i. Only fires
\* if i is reachable from j and j is behind i.
AppendOplog(i, j) ==
    /\ i # j
    /\ {i, j} \in network
    /\ state[j] = "Secondary"
    /\ Len(log[j]) < Len(log[i])
    /\ LET nextIdx == Len(log[j]) + 1
           entry   == log[i][nextIdx]
       IN  /\ \/ nextIdx = 1
              \/ log[j][nextIdx - 1].term = log[i][nextIdx - 1].term
           /\ log'   = [log   EXCEPT ![j] = Append(log[j], entry)]
           /\ store' = [store EXCEPT ![j] = ApplyEntry(store[j], entry)]
           /\ currentTerm' = [currentTerm EXCEPT
                                 ![j] = IF entry.term > currentTerm[j]
                                        THEN entry.term
                                        ELSE currentTerm[j]]
    /\ UNCHANGED <<state, network, majorityAcks, writeCount>>

\* Primary i commits its tail entry once a majority of REACHABLE
\* secondaries have it. Records a legitimate majority ack.
MajorityCommit(i) ==
    /\ state[i] = "Primary"
    /\ Len(log[i]) > 0
    /\ LET tailIdx == Len(log[i])
           entry   == log[i][tailIdx]
           agree   == {s \in Server :
                         /\ Len(log[s]) >= tailIdx
                         /\ log[s][tailIdx] = entry}
       IN  /\ IsMajority(agree)
           /\ i \in agree
           /\ entry.term = currentTerm[i]
           /\ majorityAcks' = majorityAcks \cup
                              {[key |-> entry.key,
                                value |-> entry.value,
                                term |-> entry.term,
                                server |-> i]}
    /\ UNCHANGED <<state, currentTerm, log, store, network, writeCount>>

\* Secondary j rolls back its tail entry if it conflicts with primary
\* i's view and the rolled-back entry has a lower term. Mongo-style
\* prefix rollback.
RollbackOplog(j, i) ==
    /\ i # j
    /\ {i, j} \in network
    /\ state[j] = "Secondary"
    /\ Len(log[j]) > 0
    /\ LastTerm(j) < LastTerm(i)
    /\ \/ Len(log[j]) > Len(log[i])
       \/ /\ Len(log[j]) <= Len(log[i])
          /\ log[j][Len(log[j])].term # log[i][Len(log[j])].term
    /\ LET truncated == SubSeq(log[j], 1, Len(log[j]) - 1)
       IN  /\ log'   = [log   EXCEPT ![j] = truncated]
           /\ store' = [store EXCEPT ![j] = ReplayLog(truncated)]
    /\ UNCHANGED <<state, currentTerm, network, majorityAcks, writeCount>>

\* A primary discovers (via a reachable peer) that there is a higher
\* term and steps down. This is the "scape-hatch" the bug ticket
\* discusses: forcing a no-op write would touch a peer and this action
\* would fire, breaking the no-op short-circuit's preconditions.
StepDownOnHigherTerm(i, j) ==
    /\ state[i] = "Primary"
    /\ i # j
    /\ {i, j} \in network
    /\ currentTerm[j] > currentTerm[i]
    /\ state'       = [state       EXCEPT ![i] = "Secondary"]
    /\ currentTerm' = [currentTerm EXCEPT ![i] = currentTerm[j]]
    /\ UNCHANGED <<log, store, network, majorityAcks, writeCount>>

\* A network link drops. {i, j} is removed from `network`.
PartitionLink(i, j) ==
    /\ i # j
    /\ {i, j} \in network
    /\ network' = network \ {{i, j}}
    /\ UNCHANGED <<state, currentTerm, log, store, majorityAcks,
                   writeCount>>

\* A network link heals.
HealLink(i, j) ==
    /\ i # j
    /\ {i, j} \notin network
    /\ network' = network \cup {{i, j}}
    /\ UNCHANGED <<state, currentTerm, log, store, majorityAcks,
                   writeCount>>

----
\* Next-state

Next ==
    \/ \E i \in Server : Elect(i)
    \/ \E i \in Server, k \in Key, v \in Value : ClientWrite(i, k, v)
    \/ \E i \in Server, k \in Key, v \in Value :
           NoOpMajorityAckOnStalePrimary(i, k, v)
    \/ \E i, j \in Server : AppendOplog(i, j)
    \/ \E i \in Server : MajorityCommit(i)
    \/ \E i, j \in Server : RollbackOplog(i, j)
    \/ \E i, j \in Server : StepDownOnHigherTerm(i, j)
    \/ \E i, j \in Server : PartitionLink(i, j)
    \/ \E i, j \in Server : HealLink(i, j)

Spec == Init /\ [][Next]_vars

----
\* Properties

\* Standard Raft safety: at most one primary per term.
NoTwoPrimariesInSameTerm ==
    \A i, j \in Server :
        (i # j /\ state[i] = "Primary" /\ state[j] = "Primary")
            => currentTerm[i] # currentTerm[j]

\* Quorum intersection: for every majority-ack ever returned, there
\* exists a quorum of servers whose log at the ack's term reflects that
\* (key, value). Equivalently: every majority ack is backed by a real
\* majority of replicas.
\*
\* The no-op short-circuit on a stale isolated primary violates this:
\* it records an ack while only ONE node (the isolated primary itself)
\* has the value, and that one node is not a majority.
AckBackedByMajority ==
    \A ack \in majorityAcks :
        \E quorum \in SUBSET Server :
            /\ IsMajority(quorum)
            /\ \A s \in quorum :
                  \/ \E idx \in 1..Len(log[s]) :
                        /\ log[s][idx].key   = ack.key
                        /\ log[s][idx].value = ack.value
                        /\ log[s][idx].term  = ack.term
                  \/ /\ store[s][ack.key] = ack.value
                     /\ currentTerm[s]    >= ack.term

\* Split-brain ack: two majority-acks on the SAME key with DIFFERENT
\* values at terms t1 <= t2, where the later term's value disagrees
\* with the earlier. If t1 <= t2 and ack2 is the linearised winner,
\* then ack1 was a lie.
SplitBrainAck ==
    \E a1, a2 \in majorityAcks :
        /\ a1 # a2
        /\ a1.key   = a2.key
        /\ a1.value # a2.value
        /\ a1.term  <= a2.term

NoSplitBrainAck == ~SplitBrainAck

\* State constraint to bound TLC.
StateConstraint ==
    /\ \A s \in Server : Len(log[s]) <= 3
    /\ \A s \in Server : currentTerm[s] <= 3
    /\ writeCount <= MaxWrites

============================================================================
