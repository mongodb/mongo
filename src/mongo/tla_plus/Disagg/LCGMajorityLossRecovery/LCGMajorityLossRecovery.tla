\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------- MODULE LCGMajorityLossRecovery -------------------------
\*
\* A specification of disaster-recovery orchestration for a Log Consensus Group
\* (LCG) after permanent majority loss. Models a 5-node LCG that suffers a
\* simultaneous 3-of-5 permanent loss and the orchestrated procedure that
\* reforms an LCG out of the surviving nodes plus object-store snapshots.
\*
\* The safety invariant `NoCommittedDataLoss' requires that every LSN that was
\* previously acknowledged as committed (i.e. replicated to a quorum of the
\* original LCG) is present in any successor LCG formed by the recovery
\* procedure.
\*
\* The recovery action `Recover' requires an operator attestation that the
\* survivors carry the highest-LSN among reachable copies (live nodes plus
\* object-store snapshots) before forming the new LCG. The bug configuration
\* `MCLCGMajorityLossRecoveryBug.cfg' skips that attestation; TLC then produces
\* a counter-example where the successor LCG diverges from the predecessor.
\*
\* SLS-5021 — see src/mongo/db/SLS-5021-DESIGN.md.
\*
\* To run the model-checker:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Disagg/LCGMajorityLossRecovery
\*

EXTENDS Integers, FiniteSets, Sequences, TLC

\* The set of node IDs that make up the initial LCG.
CONSTANT Node

\* The maximum LSN any node can reach during model-checking. Bounded so the
\* state space stays finite.
CONSTANT MaxLSN

\* Toggle controlling whether the recovery procedure must enforce the
\* highest-LSN operator attestation. The canonical (safe) config sets this to
\* TRUE; the bug config flips it to FALSE to force a counter-example.
CONSTANT RequireAttestation

ASSUME Cardinality(Node) = 5
ASSUME MaxLSN \in Nat
ASSUME RequireAttestation \in BOOLEAN

(**************************************************************************************************)
(* Variables                                                                                      *)
(**************************************************************************************************)

\* The current LCG membership. A subset of Node.
VARIABLE lcg

\* The set of nodes that have been permanently lost. Once lost, a node never
\* returns. losts is a subset of Node and lcg \cap losts = {} after recovery.
VARIABLE lost

\* Per-node persisted log: a function Node -> Seq(Nat). log[n][i] is the LSN
\* persisted at position i on node n.
VARIABLE log

\* Per-node object-store snapshot LSN. snapshot[n] is the highest LSN that
\* node n has uploaded to the object store. Snapshots are durable and survive
\* even when their owning node is lost.
VARIABLE snapshot

\* Set of LSNs the predecessor LCG acknowledged as committed (i.e. replicated
\* to a quorum of the original LCG before the loss). The safety invariant is
\* phrased against this set.
VARIABLE committed

\* Whether the LCG has suffered a majority loss event and is awaiting
\* recovery. The recovery action is only enabled when this is TRUE.
VARIABLE majorityLost

\* Whether the operator attestation has been recorded for this recovery. The
\* recovery action is gated on this when RequireAttestation = TRUE.
VARIABLE attested

\* The set of LCG generations seen so far. Used by the safety invariant to
\* compare predecessor-vs-successor membership.
VARIABLE generation

vars == <<lcg, lost, log, snapshot, committed, majorityLost, attested,
          generation>>

(**************************************************************************************************)
(* Helpers                                                                                        *)
(**************************************************************************************************)

\* Quorum size for the initial 5-node LCG.
Quorum == 3

\* Highest LSN persisted on node n. Returns 0 if the log is empty.
HighestLSN(n) ==
    IF Len(log[n]) = 0 THEN 0 ELSE log[n][Len(log[n])]

\* Highest LSN reachable across the surviving live nodes plus their object-
\* store snapshots. This is the LSN the operator must attest the survivors
\* carry before the new LCG is formed.
ReachableHighestLSN ==
    LET liveLogs    == { HighestLSN(n) : n \in (Node \ lost) }
        liveSnaps   == { snapshot[n]    : n \in (Node \ lost) }
        \* Snapshots from lost nodes are still durable in the object store.
        deadSnaps   == { snapshot[n]    : n \in lost }
        all         == liveLogs \cup liveSnaps \cup deadSnaps
    IN  IF all = {} THEN 0
        ELSE CHOOSE x \in all : \A y \in all : x >= y

(**************************************************************************************************)
(* Initial state                                                                                  *)
(**************************************************************************************************)

Init ==
    /\ lcg          = Node
    /\ lost         = {}
    /\ log          = [n \in Node |-> <<>>]
    /\ snapshot     = [n \in Node |-> 0]
    /\ committed    = {}
    /\ majorityLost = FALSE
    /\ attested     = FALSE
    /\ generation   = 1

(**************************************************************************************************)
(* Actions                                                                                        *)
(**************************************************************************************************)

\* [ACTION] The predecessor LCG accepts a write at the next LSN and replicates
\* it to a quorum. Enabled only while the LCG is healthy (no majority loss
\* and full membership intact).
AcceptWrite ==
    /\ ~majorityLost
    /\ Cardinality(lcg) >= Quorum
    /\ LET prevMax == CHOOSE x \in { HighestLSN(n) : n \in lcg } \cup {0} :
                          \A y \in { HighestLSN(n) : n \in lcg } \cup {0} :
                             x >= y
           nextLSN == prevMax + 1
       IN  /\ nextLSN <= MaxLSN
           /\ \E Q \in SUBSET lcg :
                /\ Cardinality(Q) >= Quorum
                /\ log' = [n \in Node |->
                             IF n \in Q THEN Append(log[n], nextLSN)
                                        ELSE log[n]]
                /\ committed' = committed \cup {nextLSN}
    /\ UNCHANGED <<lcg, lost, snapshot, majorityLost, attested, generation>>

\* [ACTION] A node uploads its currently-persisted prefix to the object store.
\* This corresponds to the PM/PS hydration path: data already majority-
\* committed is durable in S3.
Snapshot(n) ==
    /\ ~majorityLost
    /\ n \in lcg
    /\ HighestLSN(n) > snapshot[n]
    /\ snapshot' = [snapshot EXCEPT ![n] = HighestLSN(n)]
    /\ UNCHANGED <<lcg, lost, log, committed, majorityLost, attested,
                   generation>>

\* [ACTION] Simulate the permanent loss of three nodes. After this fires the
\* surviving LCG has only two nodes — below the quorum threshold — and the
\* protocol cannot make forward progress until the operator-attested recovery
\* action runs.
MajorityLoss ==
    /\ ~majorityLost
    /\ Cardinality(lost) = 0
    /\ \E S \in SUBSET lcg :
         /\ Cardinality(S) = 3
         /\ lost' = S
         /\ lcg'  = lcg \ S
    /\ majorityLost' = TRUE
    /\ UNCHANGED <<log, snapshot, committed, attested, generation>>

\* [ACTION] Operator records an attestation that the surviving nodes — taken
\* together with the durable object-store snapshots — carry the highest LSN
\* across reachable copies. The gate is: at least one survivor's on-disk log
\* OR self-owned snapshot register has reached the ceiling of reachable LSNs
\* (i.e. hydration has already pulled the data into survivor scope). In
\* production this is a runbook signal: the operator runs a CLI that
\* enumerates survivors, queries the object store for the highest snapshot,
\* fences any straggler, and writes a structured attestation event.
Attest ==
    /\ majorityLost
    /\ ~attested
    /\ LET survivorReach == { HighestLSN(n) : n \in (Node \ lost) }
                              \cup { snapshot[n] : n \in (Node \ lost) }
           survivorMax   == IF survivorReach = {} THEN 0
                            ELSE CHOOSE x \in survivorReach :
                                    \A y \in survivorReach : x >= y
       IN  survivorMax >= ReachableHighestLSN
    /\ attested' = TRUE
    /\ UNCHANGED <<lcg, lost, log, snapshot, committed, majorityLost,
                   generation>>

\* [ACTION] Reform a new LCG out of the surviving nodes after recovery. The
\* canonical config requires the attestation; the bug config skips it. When
\* RequireAttestation is FALSE, the recovery may proceed even if survivors
\* are missing committed LSNs that still live in object-store snapshots —
\* this is the path that produces the counter-example.
Recover ==
    /\ majorityLost
    /\ Cardinality(Node \ lost) >= 2 \* At least two survivors.
    /\ \/ ~RequireAttestation
       \/ attested
    /\ lcg' = Node \ lost
    /\ majorityLost' = FALSE
    /\ attested' = FALSE
    /\ generation' = generation + 1
    /\ UNCHANGED <<lost, log, snapshot, committed>>

\* [ACTION] The new LCG hydrates from the object store by replaying any
\* snapshot whose LSN exceeds the survivors' on-disk state. May fire either
\* during the recovery window (operator-driven pre-attestation hydration
\* from any reachable snapshot, including snapshots originally written by
\* now-lost nodes) or after recovery (steady-state catch-up). In the buggy
\* path Hydrate can be skipped — `Recover' alone is enough to produce the
\* counter-example.
Hydrate(n) ==
    /\ n \in (Node \ lost)
    /\ LET reachable == { snapshot[m] : m \in Node }
           ceiling   == IF reachable = {} THEN 0
                        ELSE CHOOSE x \in reachable :
                                \A y \in reachable : x >= y
       IN  /\ ceiling > HighestLSN(n)
           /\ log' = [log EXCEPT ![n] =
                         [i \in 1..ceiling |-> i]]
           \* Also lift the local snapshot register to reflect that the
           \* survivor has now ingested the object-store frontier.
           /\ snapshot' = [snapshot EXCEPT ![n] = ceiling]
    /\ UNCHANGED <<lcg, lost, committed, majorityLost, attested, generation>>

Next ==
    \/ AcceptWrite
    \/ \E n \in Node : Snapshot(n)
    \/ MajorityLoss
    \/ Attest
    \/ Recover
    \/ \E n \in Node : Hydrate(n)

Spec == Init /\ [][Next]_vars

(**************************************************************************************************)
(* Invariants                                                                                     *)
(**************************************************************************************************)

\* Type-correctness sanity check.
TypeOK ==
    /\ lcg          \subseteq Node
    /\ lost         \subseteq Node
    /\ lcg \cap lost = {}
    /\ log          \in [Node -> Seq(0..MaxLSN)]
    /\ snapshot     \in [Node -> 0..MaxLSN]
    /\ committed    \subseteq 0..MaxLSN
    /\ majorityLost \in BOOLEAN
    /\ attested     \in BOOLEAN
    /\ generation   \in Nat

\* The headline safety invariant: every LSN previously acknowledged as
\* committed by the predecessor LCG must be representable in the successor
\* LCG once recovery has settled. "Representable" means either (a) present in
\* the on-disk log of at least one current LCG member, or (b) present as a
\* durable object-store snapshot reachable to current members. While the LCG
\* is in the majority-lost state we permit the gap (the recovery has not
\* finished yet); once `majorityLost' clears, the gap must be closed.
NoCommittedDataLoss ==
    majorityLost \/
    \A lsn \in committed :
        \/ \E n \in lcg : \E i \in 1..Len(log[n]) : log[n][i] = lsn
        \/ \E n \in lcg : snapshot[n] >= lsn

\* Monotonic LSN fence: at least one current LCG member must hold (in its
\* log or its snapshot register) the highest LSN previously acknowledged as
\* committed. Models the runtime assertion that fences the LCG from accepting
\* writes below a previously-acknowledged ceiling — if no surviving member
\* has reached the ceiling, the new LCG must not be allowed to start.
MonotonicLSNFence ==
    majorityLost \/
    LET maxCommitted == IF committed = {} THEN 0
                        ELSE CHOOSE x \in committed :
                                \A y \in committed : x >= y
    IN  \/ maxCommitted = 0
        \/ \E n \in lcg : HighestLSN(n) >= maxCommitted
                        \/ snapshot[n] >= maxCommitted

\* Quorum-loss attribution: while in the lost state the active LCG must be
\* strictly below the original quorum threshold. Acts as a sanity check that
\* the model is exercising the disaster scenario, not just background churn.
QuorumLossWhileLost ==
    majorityLost => Cardinality(lcg) < Quorum

================================================================================
