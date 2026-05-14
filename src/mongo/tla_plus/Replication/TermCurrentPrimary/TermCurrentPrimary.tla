\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

--------------------------- MODULE TermCurrentPrimary ---------------------------
\* SERVER-125438: Nodes should update currentPrimaryIndex whenever we learn about
\* a new term.
\*
\* Today a node can learn about a new term via the heartbeat / log fetcher path
\* but the path only advances currentTerm without also setting
\* currentPrimaryIndex. That leaves a window during which the node will report
\* an out-of-date primary for the new term. This spec models that window and
\* checks the invariant that says: if a node knows term T, its recorded primary
\* index for term T is either Nil (unknown) or the actual primary of term T
\* (never a stale primary from an earlier term).
\*
\* The same module is used for both the green (fixed) and the bug
\* configurations. AtomicPrimaryUpdate toggles whether the term and the
\* primary index advance atomically.
\*
\* To run the model-checker:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Replication/TermCurrentPrimary

EXTENDS Integers, FiniteSets, Sequences, TLC

\* The set of server IDs.
CONSTANT Server

\* Sentinel meaning "no primary recorded for the given term".
CONSTANT Nil

\* TRUE => term and primary index update atomically (green / fix).
\* FALSE => term may advance before primary index is set (bug / before-fix).
CONSTANT AtomicPrimaryUpdate

----
\* Per-server variables.

\* The server's term number.
VARIABLE currentTerm

\* The server's state ("Follower" or "Leader").
VARIABLE state

\* The server's recorded primary index for currentTerm. Nil when not yet
\* learned. This is the field that the bug ticket calls _currentPrimaryIndex.
VARIABLE currentPrimaryIndex

----
\* Global ledger: which node (if any) actually won the election for each term.
\* For a given term t, ElectedPrimary[t] is either a server id or Nil. Once
\* set, it does not change for that term; a follow-up term has its own slot.
VARIABLE electedPrimary

vars == <<currentTerm, state, currentPrimaryIndex, electedPrimary>>

----
\* Helpers.

IsMajority(servers) == Cardinality(servers) * 2 > Cardinality(Server)

Range(f) == {f[x] : x \in DOMAIN f}

\* The largest term any node has observed so far.
GlobalCurrentTerm == LET seen == Range(currentTerm) IN
                     CHOOSE x \in seen : \A y \in seen : x >= y

\* Truthy when t has a primary recorded in the global ledger.
HasElectedPrimary(t) == /\ t \in DOMAIN electedPrimary
                       /\ electedPrimary[t] # Nil

----
\* Initial state.

Init ==
    /\ currentTerm         = [i \in Server |-> 0]
    /\ state               = [i \in Server |-> "Follower"]
    /\ currentPrimaryIndex = [i \in Server |-> Nil]
    \* Term 0 has no primary; the empty ledger gets extended on each
    \* election.
    /\ electedPrimary      = [t \in {0} |-> Nil]

----
\* Actions.

\* ACTION
\* Node i is elected by a majority of voters at a brand-new term. The new
\* term is GlobalCurrentTerm + 1 so terms are unique per election. The newly
\* elected node updates its own currentPrimaryIndex atomically with becoming
\* primary - this path is unaffected by the bug, which is on the follower
\* side.
BecomePrimary(i, voters) ==
    /\ IsMajority(voters)
    /\ i \in voters
    /\ \A v \in voters : currentTerm[v] <= GlobalCurrentTerm
    /\ LET newTerm == GlobalCurrentTerm + 1
       IN /\ currentTerm' = [s \in Server |->
                              IF s \in voters THEN newTerm ELSE currentTerm[s]]
          /\ state'       = [s \in Server |->
                              IF s = i THEN "Leader"
                              ELSE IF s \in voters THEN "Follower"
                              ELSE state[s]]
          /\ currentPrimaryIndex' =
                [s \in Server |->
                    IF s \in voters THEN i ELSE currentPrimaryIndex[s]]
          /\ electedPrimary' =
                [t \in (DOMAIN electedPrimary) \union {newTerm} |->
                    IF t = newTerm THEN i ELSE electedPrimary[t]]

\* ACTION
\* Leader i steps down. Term is unchanged; the primary index it recorded for
\* its own term is no longer accurate from its perspective, so it clears its
\* slot. Followers keep whatever primary index they had for the term.
Stepdown(i) ==
    /\ state[i] = "Leader"
    /\ state' = [state EXCEPT ![i] = "Follower"]
    /\ currentPrimaryIndex' =
            [currentPrimaryIndex EXCEPT ![i] = Nil]
    /\ UNCHANGED <<currentTerm, electedPrimary>>

\* ACTION
\* Follower i learns about a higher term from peer j via heartbeat / log
\* fetcher. This is the action the ticket is about. In the green
\* configuration (AtomicPrimaryUpdate = TRUE) the follower updates both
\* currentTerm and currentPrimaryIndex in one step using the primary that
\* peer j knows about. In the bug configuration (AtomicPrimaryUpdate =
\* FALSE) the follower advances its term but leaves its primary index
\* pointing at the stale value from the prior term.
LearnNewTerm(i, j) ==
    /\ currentTerm[j] > currentTerm[i]
    /\ currentTerm' = [currentTerm EXCEPT ![i] = currentTerm[j]]
    /\ state' = [state EXCEPT ![i] = "Follower"]
    /\ IF AtomicPrimaryUpdate
       THEN currentPrimaryIndex' =
                [currentPrimaryIndex EXCEPT
                    ![i] = currentPrimaryIndex[j]]
       ELSE UNCHANGED currentPrimaryIndex
    /\ UNCHANGED electedPrimary

\* ACTION
\* Follower i catches up its primary index from j without crossing a term
\* boundary. This models the slow path that today eventually corrects the
\* stale primary index. It only fires when i and j are on the same term and
\* j has a non-Nil primary recorded.
LearnPrimaryIndex(i, j) ==
    /\ currentTerm[i] = currentTerm[j]
    /\ currentPrimaryIndex[j] # Nil
    /\ currentPrimaryIndex[i] # currentPrimaryIndex[j]
    /\ currentPrimaryIndex' =
            [currentPrimaryIndex EXCEPT
                ![i] = currentPrimaryIndex[j]]
    /\ UNCHANGED <<currentTerm, state, electedPrimary>>

----
\* Action groupings.

BecomePrimaryAction ==
    \E i \in Server : \E voters \in SUBSET Server : BecomePrimary(i, voters)

StepdownAction ==
    \E i \in Server : Stepdown(i)

LearnNewTermAction ==
    \E i, j \in Server : LearnNewTerm(i, j)

LearnPrimaryIndexAction ==
    \E i, j \in Server : LearnPrimaryIndex(i, j)

Next ==
    \/ BecomePrimaryAction
    \/ StepdownAction
    \/ LearnNewTermAction
    \/ LearnPrimaryIndexAction

SpecBehavior == Init /\ [][Next]_vars

Fairness ==
    /\ WF_vars(LearnPrimaryIndexAction)
    /\ WF_vars(LearnNewTermAction)

Spec == SpecBehavior /\ Fairness

----
\* Properties.

\* SAFETY
\* If a node has observed term t and has any primary recorded for t, that
\* primary must match the real elected primary of term t. A node may legally
\* hold Nil while it has not yet learned the primary, but it must not point
\* at a stale primary from an earlier term.
PrimaryIndexConsistentWithTerm ==
    \A i \in Server :
        LET t == currentTerm[i]
            p == currentPrimaryIndex[i]
        IN
            \/ p = Nil
            \/ /\ t \in DOMAIN electedPrimary
               /\ electedPrimary[t] # Nil
               /\ p = electedPrimary[t]

\* LIVENESS
\* Once a term has an elected primary recorded in the ledger, every node
\* that has caught up to that term eventually learns the right primary
\* index. With AtomicPrimaryUpdate this is immediate; without it, the slow
\* path (LearnPrimaryIndex) is what restores the invariant.
EveryNodeEventuallyLearnsCurrentPrimary ==
    \A i \in Server :
        [](currentPrimaryIndex[i] = Nil /\ HasElectedPrimary(currentTerm[i])
            ~> currentPrimaryIndex[i] = electedPrimary[currentTerm[i]])

=============================================================================
