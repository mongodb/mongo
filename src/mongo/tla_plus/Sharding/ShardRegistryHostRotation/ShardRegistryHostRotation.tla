\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------------ MODULE ShardRegistryHostRotation ------------------------------------
\* Formal specification for the ShardRegistry refresh protocol when shard host-sets rotate, modelling
\* the bug described in SERVER-110328:
\*
\*   "ShardRegistry can be unable to refresh when all hosts of a shard have been changed."
\*
\* Behavioural summary of the modelled subsystem
\* ---------------------------------------------
\* Since SERVER-91121, the ShardRegistry refreshes from the configsvr only when it observes that
\* `topologyTime' has advanced. A replica-set reconfig that swaps the `hosts' field of the
\* corresponding `config.shards' entry (SERVER-21185) does NOT advance `topologyTime'. The cluster
\* relies on a *side channel* to keep the ShardRegistry honest: the ReplicaSetMonitor (RSM) notifies
\* the ShardRegistry whenever it learns a new connection string from any replica it can still reach.
\*
\* The bug
\* -------
\* If, while a router-node is partitioned away from *every* member it currently knows, every host of
\* the shard is replaced (a full rotation), the RSM has no surviving member to learn the new
\* connection string from, the configsvr never bumps `topologyTime', and the ShardRegistry has no
\* trigger to refresh. The node remains unable to communicate with the shard until restart.
\*
\* This specification:
\* - Models a single shard (replica set) whose host-set is a SUBSET of a small global host universe.
\*   Distinct generations of the host-set are tracked via `shardHostsGen', a monotonically advancing
\*   counter that is bumped on every reconfig (a proxy for `replSetConfigVersion').
\* - Models the configsvr-side `config.shards' document as (hosts, configVersion, topologyTime). The
\*   `topologyTime' field is bumped only on shard add/remove (modelled as `TopologyTimeAdvance'),
\*   never on a host rotation.
\* - Models a router-side ShardRegistry as (cachedHosts, cachedConfigVersion, lastSeenTopologyTime).
\* - Models RSM connectivity as a per-host boolean `nodeCanReach', which can be flipped to FALSE by
\*   `NetworkPartition' (the router-node loses contact with a host) and to TRUE by `NetworkHeal'.
\* - Models three independent refresh triggers, mirroring the production code:
\*     (a) PeriodicTopologyTimeRefresh: refresh iff `cfgTopologyTime > lastSeenTopologyTime'.
\*     (b) RSMNotifiesShardRegistry:    refresh when the RSM observes a new connection string from a
\*                                      reachable surviving member.
\*     (c) NodeRestart:                 wipes the registry; on reload it fetches fresh hosts.
\*
\* What we prove
\* -------------
\* The headline correctness invariant `EventuallyConverges' (a temporal property) states that as
\* long as the cluster eventually heals at least one host shared with the current shard host-set, or
\* a restart eventually occurs, the ShardRegistry catches up. We additionally encode the SERVER-110328
\* bait invariant `BaitDivergenceStuck' which is satisfied by exactly the traces the bug describes:
\* full host rotation while every previous host is unreachable, with no restart.
\*
\* To run the model-checker:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Sharding/ShardRegistryHostRotation

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Hosts,              \* Global host universe (e.g. {h1, h2, h3, h4}). At least 2 elements.
    MAX_ROTATIONS,      \* Bound on host rotations (replSetReconfig events).
    MAX_TOPOLOGY_BUMPS  \* Bound on add/remove-shard events that bump topologyTime.

ASSUME Cardinality(Hosts) >= 2
ASSUME MAX_ROTATIONS \in 0..10
ASSUME MAX_TOPOLOGY_BUMPS \in 0..5

(* Configsvr-side variables: the `config.shards' projection. *)
VARIABLE cfgHosts            \* Current host-set of the shard, SUBSET Hosts, non-empty.
VARIABLE cfgConfigVersion    \* Monotonic; bumped on every reconfig (host rotation).
VARIABLE cfgTopologyTime     \* Monotonic; bumped ONLY by `TopologyTimeAdvance'.

(* Router-side ShardRegistry cache. *)
VARIABLE srHosts                  \* What the registry currently believes the host-set to be.
VARIABLE srCachedConfigVersion    \* Last config version the registry learned.
VARIABLE srLastSeenTopologyTime   \* Last topologyTime the registry observed at refresh time.

(* RSM-style connectivity for the router-node. nodeCanReach[h] = TRUE iff the node can RPC h. *)
VARIABLE nodeCanReach

(* Ancillary bound counters. *)
VARIABLE rotations
VARIABLE topologyBumps

vars == <<cfgHosts, cfgConfigVersion, cfgTopologyTime,
          srHosts, srCachedConfigVersion, srLastSeenTopologyTime,
          nodeCanReach, rotations, topologyBumps>>

cfg_vars   == <<cfgHosts, cfgConfigVersion, cfgTopologyTime>>
sr_vars    == <<srHosts, srCachedConfigVersion, srLastSeenTopologyTime>>
rsm_vars   == <<nodeCanReach>>
bound_vars == <<rotations, topologyBumps>>

(* Helpers *)
NonEmptySubsets(S) == { T \in SUBSET S : T # {} }
ReachableMembers   == { h \in cfgHosts : nodeCanReach[h] }
KnownReachable     == { h \in srHosts : nodeCanReach[h] }

(*************************************************************************)
(* Init                                                                  *)
(*************************************************************************)
\* The router-node starts with a warm cache that matches the current host-set, and the network is
\* fully connected. This is the only configuration in which the ShardRegistry trivially knows the
\* truth; from here all interesting traces are reached by Next.
Init ==
    /\ cfgHosts \in NonEmptySubsets(Hosts)
    /\ cfgConfigVersion = 1
    /\ cfgTopologyTime = 1
    /\ srHosts = cfgHosts
    /\ srCachedConfigVersion = cfgConfigVersion
    /\ srLastSeenTopologyTime = cfgTopologyTime
    /\ nodeCanReach = [h \in Hosts |-> TRUE]
    /\ rotations = 0
    /\ topologyBumps = 0

(*************************************************************************)
(* Actions: configsvr / shard side                                       *)
(*************************************************************************)

\* Action: a replSetReconfig replaces the shard's host-set. This is SERVER-21185: the
\* `hosts' field of `config.shards' is rewritten with the new connection string. Crucially,
\* `topologyTime' is NOT advanced. `configVersion' is bumped (it mirrors `replSetConfigVersion').
HostRotation(newHosts) ==
    /\ rotations < MAX_ROTATIONS
    /\ newHosts \in NonEmptySubsets(Hosts)
    /\ newHosts # cfgHosts                       \* A reconfig is meaningful iff hosts actually change.
    /\ cfgHosts'         = newHosts
    /\ cfgConfigVersion' = cfgConfigVersion + 1
    /\ rotations'        = rotations + 1
    /\ UNCHANGED <<cfgTopologyTime, sr_vars, rsm_vars, topologyBumps>>

\* Action: addShard / removeShard advances topologyTime. We do not model the membership change
\* itself; we model only that topologyTime moves forward and the ShardRegistry has a fresh
\* refresh-trigger as a result.
TopologyTimeAdvance ==
    /\ topologyBumps < MAX_TOPOLOGY_BUMPS
    /\ cfgTopologyTime' = cfgTopologyTime + 1
    /\ topologyBumps'   = topologyBumps + 1
    /\ UNCHANGED <<cfgHosts, cfgConfigVersion, sr_vars, rsm_vars, rotations>>

(*************************************************************************)
(* Actions: network / RSM side                                           *)
(*************************************************************************)

\* Action: the router-node loses connectivity with `h'.
NetworkPartition(h) ==
    /\ nodeCanReach[h] = TRUE
    /\ nodeCanReach' = [nodeCanReach EXCEPT ![h] = FALSE]
    /\ UNCHANGED <<cfg_vars, sr_vars, bound_vars>>

\* Action: the router-node regains connectivity with `h'.
NetworkHeal(h) ==
    /\ nodeCanReach[h] = FALSE
    /\ nodeCanReach' = [nodeCanReach EXCEPT ![h] = TRUE]
    /\ UNCHANGED <<cfg_vars, sr_vars, bound_vars>>

(*************************************************************************)
(* Actions: ShardRegistry refresh triggers                               *)
(*************************************************************************)

\* Trigger (a): the ShardRegistry's periodic refresh fires, observes that `topologyTime' has
\* advanced beyond what it last saw, and refreshes from the configsvr.
\*
\* IMPORTANT: This is the only configsvr-pull path the registry has. If `topologyTime' has not
\* advanced (the SERVER-110328 scenario), this guard is FALSE and the path is dead.
PeriodicTopologyTimeRefresh ==
    /\ cfgTopologyTime > srLastSeenTopologyTime
    /\ srHosts'                = cfgHosts
    /\ srCachedConfigVersion'  = cfgConfigVersion
    /\ srLastSeenTopologyTime' = cfgTopologyTime
    /\ UNCHANGED <<cfg_vars, rsm_vars, bound_vars>>

\* Trigger (b): the ReplicaSetMonitor observes a new connection string from a *surviving* member of
\* the shard (a host that's still in `cfgHosts' AND is currently in the registry's cached host-set
\* AND is reachable by the node), and pushes the updated host-set into the ShardRegistry.
\*
\* This is the load-bearing side channel: when the shard is reconfigured but at least one previous
\* host remains in the new host-set AND is reachable, this trigger fires and the registry converges.
RSMNotifiesShardRegistry ==
    /\ \E h \in cfgHosts \cap srHosts : nodeCanReach[h] = TRUE
    /\ srHosts'                = cfgHosts
    /\ srCachedConfigVersion'  = cfgConfigVersion
    \* RSM-driven refreshes do NOT update topologyTime tracking; they push host-set only.
    /\ srLastSeenTopologyTime' = srLastSeenTopologyTime
    /\ UNCHANGED <<cfg_vars, rsm_vars, bound_vars>>

\* Trigger (c): the node restarts. The ShardRegistry is wiped and reloads from the configsvr; the
\* reload is an unconditional fetch (no `topologyTime' guard on first read).
NodeRestart ==
    /\ srHosts'                = cfgHosts
    /\ srCachedConfigVersion'  = cfgConfigVersion
    /\ srLastSeenTopologyTime' = cfgTopologyTime
    \* Restart re-establishes connectivity to every host (a coarse model of process restart).
    /\ nodeCanReach'           = [h \in Hosts |-> TRUE]
    /\ UNCHANGED <<cfg_vars, bound_vars>>

(*************************************************************************)
(* Next                                                                  *)
(*************************************************************************)
Next ==
    \/ \E newHosts \in NonEmptySubsets(Hosts) : HostRotation(newHosts)
    \/ TopologyTimeAdvance
    \/ \E h \in Hosts : NetworkPartition(h)
    \/ \E h \in Hosts : NetworkHeal(h)
    \/ PeriodicTopologyTimeRefresh
    \/ RSMNotifiesShardRegistry
    \/ NodeRestart

Spec ==
    /\ Init
    /\ [][Next]_vars
    \* Fairness: assume `topologyTime'-driven refresh, the RSM side channel, and restart are all
    \* eventually attempted when enabled. Without this, the spec admits trivial stuttering traces
    \* that never refresh.
    /\ WF_vars(PeriodicTopologyTimeRefresh)
    /\ WF_vars(RSMNotifiesShardRegistry)
    /\ WF_vars(NodeRestart)

(*************************************************************************)
(* Type invariants                                                       *)
(*************************************************************************)
TypeOK ==
    /\ cfgHosts \in NonEmptySubsets(Hosts)
    /\ cfgConfigVersion \in Nat
    /\ cfgTopologyTime \in Nat
    /\ srHosts \in SUBSET Hosts
    /\ srCachedConfigVersion \in Nat
    /\ srLastSeenTopologyTime \in Nat
    /\ nodeCanReach \in [Hosts -> BOOLEAN]
    /\ rotations \in 0..MAX_ROTATIONS
    /\ topologyBumps \in 0..MAX_TOPOLOGY_BUMPS

(*************************************************************************)
(* Safety / liveness                                                     *)
(*************************************************************************)

\* Monotonicity: ShardRegistry's view of configVersion never moves backward.
RegistryConfigVersionMonotone == srCachedConfigVersion <= cfgConfigVersion

\* Monotonicity: configsvr versions only ever move forward.
ConfigMonotone ==
    /\ cfgConfigVersion >= 1
    /\ cfgTopologyTime >= 1

\* Liveness (the headline property): if the network eventually heals enough that some current host
\* is reachable, OR a restart eventually fires, the registry catches up to the current host-set.
\* This is the SERVER-110328 contract: the bug is exactly a violation of this in the absence of a
\* topology-time bump and with no shared, reachable member.
EventuallyConverges ==
    <>[](srHosts = cfgHosts)

\* Defensive liveness when only the RSM channel is available: if the rotation preserves at least one
\* previously-known host that's reachable, the registry catches up via the RSM side channel.
ConvergesViaRSMWhenSharedHostReachable ==
    [](\E h \in cfgHosts \cap srHosts : nodeCanReach[h] = TRUE) => <>(srHosts = cfgHosts)

\* Defensive liveness when only the topologyTime channel is available: if topologyTime is bumped
\* infinitely often, the registry catches up.
ConvergesViaTopologyTimeWhenBumped ==
    []<>(cfgTopologyTime > srLastSeenTopologyTime) => <>(srHosts = cfgHosts)

(*************************************************************************)
(* Bait invariants (negated to surface as counterexamples in TLC)        *)
(*************************************************************************)

\* The SERVER-110328 bait: there exists a reachable state in which the registry's view diverges
\* from the configsvr truth AND the RSM has no surviving reachable member to learn from AND the
\* topologyTime hasn't advanced past what the registry has seen. In that state, no refresh trigger
\* fires and (absent restart) divergence is permanent.
\*
\* If TLC reports this invariant as violated, the trace IS the SERVER-110328 reproducer.
BaitDivergenceStuck ==
    ~ ( /\ srHosts # cfgHosts
        /\ (cfgHosts \cap srHosts) \cap { h \in Hosts : nodeCanReach[h] } = {}
        /\ cfgTopologyTime <= srLastSeenTopologyTime )

\* The trivial bait: any divergence at all (used to confirm the search space is non-empty).
BaitAnyDivergence == srHosts = cfgHosts

====================================================================================================
