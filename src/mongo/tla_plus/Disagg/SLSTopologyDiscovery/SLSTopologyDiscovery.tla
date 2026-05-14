\* Copyright 2025 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------------- MODULE SLSTopologyDiscovery ----------------------------------------
\* This specification models automatic discovery of SLS (Storage Layer Services) topology, replacing
\* a fully-static configuration with a bootstrap-set seed + gossip-style propagation protocol.
\*
\* Motivation (SERVER-111844):
\*   SLS topology -- which peers, which cells, which log servers -- is presently specified in static
\*   disagg config. When the static spec drifts from the live cluster (e.g. due to a Kubernetes
\*   StatefulSet roll), reads/writes can target stale or non-existent peers. This spec models a
\*   discovery protocol where each discoverer node is given an initial bootstrap set (e.g. from the
\*   config server or a Kubernetes headless service) and then converges to the live authorized
\*   topology by exchanging views with already-known peers.
\*
\* Model:
\*   - Authorized is the ground-truth set of peers admitted by the config-server/admission policy.
\*   - Each discoverer holds (known, pending) sets. `known' is its current topology cache; `pending'
\*     are peers heard about but not yet probed.
\*   - Reachable is a (possibly proper) subset of Authorized representing peers reachable from a
\*     given discoverer under the current network partition.
\*   - Gossip exchanges a peer's `known' set with another peer's known set, filtered through an
\*     authorization predicate (the config server's view, which is taken as the ground truth on the
\*     authorization question).
\*
\* Safety:
\*   - EveryDiscoveredPeerIsAuthorized: discovery never adds an unauthorized peer to `known'.
\*
\* Liveness (under fairness + persistent reachability):
\*   - EveryReachablePeerEventuallyDiscovered: every discoverer eventually has a `known' set that
\*     covers the reachable subset of Authorized.
\*
\* To run the model-checker:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Disagg/SLSTopologyDiscovery

EXTENDS Integers, FiniteSets, TLC

CONSTANTS
    Peers,              \* Universe of peer identities the system could possibly see.
    Discoverers,        \* Nodes that need to learn the topology. Discoverers \subseteq Peers.
    Authorized,         \* Ground-truth set of peers admitted by config-server policy.
    BootstrapSet,       \* Initial seeds each discoverer starts with. Must \subseteq Authorized.
    Unauthorized,       \* A poisoned set: peers gossip may suggest but that are rejected.
    Reachable           \* Peers actually reachable on the network. \subseteq Authorized.

ASSUME Cardinality(Peers) > 0
ASSUME Discoverers \subseteq Peers
ASSUME Authorized \subseteq Peers
ASSUME BootstrapSet \subseteq Authorized
ASSUME Unauthorized \subseteq Peers
ASSUME Unauthorized \cap Authorized = {}
ASSUME Reachable \subseteq Authorized

VARIABLES
    known,              \* known[d]   : SUBSET Peers -- discoverer d's topology cache.
    pending,            \* pending[d] : SUBSET Peers -- peers heard of but not yet probed.
    rumors              \* rumors     : SUBSET Peers -- bag of unauthorized peers an attacker /
                        \*              stale-config source may inject via gossip.

vars == <<known, pending, rumors>>

(**************************************************************************************************)
(* Initial state.                                                                                 *)
(*                                                                                                *)
(* Each discoverer starts with the bootstrap set (e.g. config-server-issued seeds) already        *)
(* validated as authorized. `pending' begins empty: discoverers must hear about non-bootstrap     *)
(* peers via gossip. `rumors' is an arbitrary subset of Unauthorized (allowing the model checker  *)
(* to explore all attacker-injected gossip).                                                      *)
(**************************************************************************************************)
Init ==
    /\ known   = [d \in Discoverers |-> BootstrapSet]
    /\ pending = [d \in Discoverers |-> {}]
    /\ rumors \in SUBSET Unauthorized

(**************************************************************************************************)
(* Authorization gate.                                                                            *)
(*                                                                                                *)
(* This represents the config-server / admission-policy check that runs before a gossiped peer is *)
(* added to the topology cache. It is taken as the ground truth: a peer is allowed iff it is in   *)
(* the Authorized set.                                                                            *)
(**************************************************************************************************)
IsAuthorized(p) == p \in Authorized

(**************************************************************************************************)
(* Action: GossipPull.                                                                            *)
(*                                                                                                *)
(* Discoverer d contacts an already-known, reachable peer src and pulls src's view of the         *)
(* topology. In the model, src may also be a discoverer (peers gossip with each other). We take   *)
(* the union of src's known set and (non-deterministically) some subset of rumors -- modelling    *)
(* the possibility that the contacted peer itself has stale or attacker-influenced data. The      *)
(* incoming set is then dropped into `pending' for later authorization, NOT directly into         *)
(* `known'.                                                                                       *)
(**************************************************************************************************)
GossipPull(d, src) ==
    /\ d \in Discoverers
    /\ src \in known[d]                     \* Must already know src to talk to it.
    /\ src \in Reachable                    \* And src must be currently reachable.
    /\ \E injected \in SUBSET rumors :
        LET srcView ==
                IF src \in Discoverers
                    THEN known[src] \cup injected
                    ELSE Authorized \cap Reachable \cup injected
                                            \* Non-discoverer peers report their own neighbours.
            newPending == (srcView \ known[d])
        IN  /\ pending' = [pending EXCEPT ![d] = @ \cup newPending]
            /\ UNCHANGED <<known, rumors>>

(**************************************************************************************************)
(* Action: AdmitPeer.                                                                             *)
(*                                                                                                *)
(* Drain one peer from pending into known, gated by the authorization predicate. This is the      *)
(* single safety-critical step: unauthorized peers are dropped on the floor, never enter `known'. *)
(**************************************************************************************************)
AdmitPeer(d, p) ==
    /\ d \in Discoverers
    /\ p \in pending[d]
    /\ pending' = [pending EXCEPT ![d] = @ \ {p}]
    /\ IF IsAuthorized(p)
        THEN known' = [known EXCEPT ![d] = @ \cup {p}]
        ELSE UNCHANGED <<known>>
    /\ UNCHANGED <<rumors>>

(**************************************************************************************************)
(* Action: ExpirePeer.                                                                            *)
(*                                                                                                *)
(* Topology cache TTL expiry: a peer that has fallen out of the authorized set (e.g. cordoned by  *)
(* operator action) eventually drops from a discoverer's known set on the next refresh. We model  *)
(* this conservatively: only peers no longer in Authorized may expire, and only if the peer is    *)
(* not in the bootstrap set (bootstrap seeds are managed by the config server, not by TTL).      *)
(**************************************************************************************************)
ExpirePeer(d, p) ==
    /\ d \in Discoverers
    /\ p \in known[d]
    /\ p \notin Authorized
    /\ p \notin BootstrapSet
    /\ known' = [known EXCEPT ![d] = @ \ {p}]
    /\ UNCHANGED <<pending, rumors>>

(**************************************************************************************************)
(* Action: InjectRumor.                                                                           *)
(*                                                                                                *)
(* Models an attacker / stale-config source that pushes a previously-unseen unauthorized peer     *)
(* into the gossip bag. Used to exercise the authorization gate in AdmitPeer.                     *)
(**************************************************************************************************)
InjectRumor(p) ==
    /\ p \in Unauthorized
    /\ rumors' = rumors \cup {p}
    /\ UNCHANGED <<known, pending>>

Next ==
    \/ \E d \in Discoverers, src \in Peers : GossipPull(d, src)
    \/ \E d \in Discoverers, p \in Peers   : AdmitPeer(d, p)
    \/ \E d \in Discoverers, p \in Peers   : ExpirePeer(d, p)
    \/ \E p \in Peers                       : InjectRumor(p)

(**************************************************************************************************)
(* Fairness.                                                                                      *)
(*                                                                                                *)
(* For liveness we require that each discoverer keeps gossiping with already-known reachable      *)
(* peers, and that admission of pending peers eventually fires. Rumor injection and expiry are    *)
(* not required for liveness of the main discovery property.                                      *)
(**************************************************************************************************)
Fairness ==
    /\ \A d \in Discoverers, src \in Peers : WF_vars(GossipPull(d, src))
    /\ \A d \in Discoverers, p \in Peers   : WF_vars(AdmitPeer(d, p))

Spec == Init /\ [][Next]_vars /\ Fairness

----------------------------------------------------------------------------------------------------
(**************************************************************************************************)
(* Type invariants                                                                                *)
(**************************************************************************************************)
TypeOK ==
    /\ known   \in [Discoverers -> SUBSET Peers]
    /\ pending \in [Discoverers -> SUBSET Peers]
    /\ rumors  \in SUBSET Peers

(**************************************************************************************************)
(* Safety properties                                                                              *)
(**************************************************************************************************)

\* Safety #1 (required by SERVER-111844): discovery never returns an unauthorized peer.
EveryDiscoveredPeerIsAuthorized ==
    \A d \in Discoverers : known[d] \subseteq Authorized

\* Safety #2: the bootstrap set is sticky -- once seeded, always at least seeded (unless those
\* seeds are themselves de-authorized by the config server, in which case ExpirePeer is permitted).
BootstrapStickiness ==
    \A d \in Discoverers : (BootstrapSet \cap Authorized) \subseteq known[d]

\* Safety #3: rumors never leak into known directly. (Implied by EveryDiscoveredPeerIsAuthorized
\* + Unauthorized \cap Authorized = {}, but checked explicitly for clarity.)
RumorsIsolated ==
    \A d \in Discoverers : known[d] \cap rumors = {}

(**************************************************************************************************)
(* Liveness properties                                                                            *)
(**************************************************************************************************)

\* Liveness #1 (required by SERVER-111844): every reachable authorized peer is eventually known to
\* every discoverer. Convergence is guaranteed only over the reachable subset; peers behind a
\* permanent partition are not required to be discovered.
EveryReachablePeerEventuallyDiscovered ==
    \A d \in Discoverers :
        <>[](Reachable \subseteq known[d])

\* Liveness #2: peers that fall out of Authorized eventually fall out of every discoverer's known
\* set as well (closes the staleness loop). This requires fairness on ExpirePeer, which we DO NOT
\* assume in the main spec -- callers who want to verify this property should add WF on
\* ExpirePeer in their MC config.
NoStalePeersForever ==
    \A d \in Discoverers, p \in Peers :
        (p \in known[d] /\ p \notin Authorized /\ p \notin BootstrapSet) ~> (p \notin known[d])

====================================================================================================
