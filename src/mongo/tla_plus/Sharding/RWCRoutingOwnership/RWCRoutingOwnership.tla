\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------------ MODULE RWCRoutingOwnership ------------------------------
\* This specification models the propagation of the cluster-wide default read/write concern
\* (CWRWC) after ownership of the CWRWC subsystem has been reassigned from the rw-concerns
\* team to the catalog-and-routing (R&T) team (SERVER-122618).
\*
\* The ownership change is non-functional, but the resulting topology places the CWRWC
\* document on the same propagation substrate that R&T uses for routing tables and
\* topologyTime. This spec captures the invariants that must continue to hold under that
\* substrate:
\*   1. CWRWC updates are linearizable through the config shard (single writer).
\*   2. CWRWC is gossiped to mongos and shards via the same cluster-time + topologyTime
\*      vector used by routing-table refreshes (monotonic, never goes backwards).
\*   3. A CRUD operation that resolves a default RWC always observes a value no older than
\*      the routing-table generation it used to target shards (causal coupling).
\*   4. Stale CWRWC at a routing node forces a refresh on the same code path as a stale
\*      routing table (StaleConfig / StaleClusterTime), not a separate refresh path.
\*
\* Reference: src/mongo/db/s/README_routing.md (cluster-time + topologyTime gossip),
\*            src/mongo/db/read_write_concern_defaults.h (CWRWC cache).
\*
\* To run the model-checker:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh RWCRoutingOwnership

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Routers,          \* Set of mongos (routing nodes).
    Shards,           \* Set of data-bearing shards.
    ConfigShard,      \* Singleton CSRS authoritative for CWRWC + routing table.
    MaxRWCUpdates,    \* Bound on setDefaultRWConcern admin command invocations.
    MaxOperations     \* Bound on CRUD operations that resolve CWRWC.

ASSUME Cardinality(Routers) > 0
ASSUME Cardinality(Shards) > 0
ASSUME Cardinality(ConfigShard) = 1
ASSUME MaxRWCUpdates \in 0..10
ASSUME MaxOperations \in 0..10

\* Sentinel values.
NoRWC == [epoch |-> 0, value |-> "uninitialized"]
NoTopologyTime == 0

\* Generate the universe of distinct CWRWC values the spec considers. epoch is the logical
\* generation; value is opaque (the spec doesn't reason about majority/local semantics, only
\* about which version is observed where).
RWCValues == {[epoch |-> e, value |-> v] :
                  e \in 1..MaxRWCUpdates,
                  v \in {"local", "majority", "linearizable"}}

(* Authoritative state on the config shard *)
VARIABLE csRWC               \* The current CWRWC document on the CSRS.
VARIABLE csTopologyTime      \* The topologyTime advanced on every routing or CWRWC update.
                             \* Co-located substrate is the load-bearing assumption of this
                             \* spec: one monotonic clock governs both.

(* Per-router cache state *)
VARIABLE rCachedRWC          \* Router's last-seen CWRWC, per router.
VARIABLE rCachedTopologyTime \* Router's last-seen topologyTime, per router.

(* Per-shard cache state — shards also resolve CWRWC for embedded routers and txn
   coordinators *)
VARIABLE sCachedRWC
VARIABLE sCachedTopologyTime

(* Operation log: every CRUD that resolved a CWRWC, stamped with the (rwc, topologyTime)
   the router used. Used to express the causal-coupling invariant. *)
VARIABLE opLog

(* Ancillary bounds *)
VARIABLE updateCount         \* Number of setDefaultRWConcern invocations issued so far.
VARIABLE opCount             \* Number of resolved CRUD ops so far.

vars == <<csRWC, csTopologyTime, rCachedRWC, rCachedTopologyTime,
          sCachedRWC, sCachedTopologyTime, opLog, updateCount, opCount>>

config_vars == <<csRWC, csTopologyTime>>
router_vars == <<rCachedRWC, rCachedTopologyTime>>
shard_vars == <<sCachedRWC, sCachedTopologyTime>>

----------------------------------------------------------------------------
\* Type invariant.
TypeOK ==
    /\ csRWC \in RWCValues \cup {NoRWC}
    /\ csTopologyTime \in 0..(MaxRWCUpdates * 2)
    /\ rCachedRWC \in [Routers -> RWCValues \cup {NoRWC}]
    /\ rCachedTopologyTime \in [Routers -> 0..(MaxRWCUpdates * 2)]
    /\ sCachedRWC \in [Shards -> RWCValues \cup {NoRWC}]
    /\ sCachedTopologyTime \in [Shards -> 0..(MaxRWCUpdates * 2)]
    /\ updateCount \in 0..MaxRWCUpdates
    /\ opCount \in 0..MaxOperations

----------------------------------------------------------------------------
\* Initial state: CSRS holds NoRWC; every cache is empty; no ops have run.
Init ==
    /\ csRWC = NoRWC
    /\ csTopologyTime = NoTopologyTime
    /\ rCachedRWC = [r \in Routers |-> NoRWC]
    /\ rCachedTopologyTime = [r \in Routers |-> NoTopologyTime]
    /\ sCachedRWC = [s \in Shards |-> NoRWC]
    /\ sCachedTopologyTime = [s \in Shards |-> NoTopologyTime]
    /\ opLog = <<>>
    /\ updateCount = 0
    /\ opCount = 0

----------------------------------------------------------------------------
\* Action: admin issues setDefaultRWConcern against the CSRS. The CSRS is the single
\* writer; epoch and topologyTime both advance monotonically. This action models the
\* "linearizable through CSRS" requirement.
SetDefaultRWConcern(newValue) ==
    /\ updateCount < MaxRWCUpdates
    /\ newValue \in RWCValues
    /\ newValue.epoch = updateCount + 1
    /\ csRWC' = newValue
    /\ csTopologyTime' = csTopologyTime + 1
    /\ updateCount' = updateCount + 1
    /\ UNCHANGED <<rCachedRWC, rCachedTopologyTime, sCachedRWC, sCachedTopologyTime,
                   opLog, opCount>>

\* Action: a router refreshes its CWRWC cache by gossiping from the CSRS. This uses the
\* same code path as a routing-table refresh under the new ownership model — the router
\* observes (csRWC, csTopologyTime) atomically. A router may refresh at any time but cache
\* contents are monotonic in topologyTime.
RouterRefresh(r) ==
    /\ csTopologyTime > rCachedTopologyTime[r]
    /\ rCachedRWC' = [rCachedRWC EXCEPT ![r] = csRWC]
    /\ rCachedTopologyTime' = [rCachedTopologyTime EXCEPT ![r] = csTopologyTime]
    /\ UNCHANGED <<csRWC, csTopologyTime, sCachedRWC, sCachedTopologyTime,
                   opLog, updateCount, opCount>>

\* Action: a shard refreshes its CWRWC cache. Same monotonicity discipline.
ShardRefresh(s) ==
    /\ csTopologyTime > sCachedTopologyTime[s]
    /\ sCachedRWC' = [sCachedRWC EXCEPT ![s] = csRWC]
    /\ sCachedTopologyTime' = [sCachedTopologyTime EXCEPT ![s] = csTopologyTime]
    /\ UNCHANGED <<csRWC, csTopologyTime, rCachedRWC, rCachedTopologyTime,
                   opLog, updateCount, opCount>>

\* Action: a router resolves a CRUD op that has no explicit RWC and falls back to CWRWC.
\* The resolved (rwc, topologyTime) pair is logged for invariant checking. The router uses
\* whatever it has cached — if its cache is stale, this models the legitimate
\* eventual-consistency window. The invariants below pin what "stale" is allowed to mean.
ResolveOp(r) ==
    /\ opCount < MaxOperations
    /\ rCachedRWC[r] /= NoRWC
    /\ opLog' = Append(opLog, [router |-> r,
                               rwc |-> rCachedRWC[r],
                               topologyTime |-> rCachedTopologyTime[r]])
    /\ opCount' = opCount + 1
    /\ UNCHANGED <<csRWC, csTopologyTime, rCachedRWC, rCachedTopologyTime,
                   sCachedRWC, sCachedTopologyTime, updateCount>>

----------------------------------------------------------------------------
Next ==
    \/ \E v \in RWCValues : SetDefaultRWConcern(v)
    \/ \E r \in Routers : RouterRefresh(r)
    \/ \E s \in Shards : ShardRefresh(s)
    \/ \E r \in Routers : ResolveOp(r)

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------
\* INVARIANTS

\* 1. Single-writer linearizability: every observed RWC value originated at the CSRS, and
\*    epochs are uniquely assigned. There is no value with epoch > updateCount in any
\*    cache or op log.
SingleWriterLinearizable ==
    /\ csRWC.epoch <= updateCount
    /\ \A r \in Routers : rCachedRWC[r].epoch <= updateCount
    /\ \A s \in Shards : sCachedRWC[s].epoch <= updateCount
    /\ \A i \in 1..Len(opLog) : opLog[i].rwc.epoch <= updateCount

\* 2. Cache monotonicity under co-located substrate: a cache's topologyTime never
\*    decreases between successive states. (Pinned by RouterRefresh / ShardRefresh
\*    enabling conditions; checked here as a state predicate over the most-recent state.)
\*    More importantly, the rwc and topologyTime advance together at every cache: if a
\*    cache's topologyTime is t, its rwc.epoch equals the epoch that was current on the
\*    CSRS at topologyTime t — i.e., epoch matches topologyTime when both are non-zero
\*    (since every advance is paired in SetDefaultRWConcern).
RWCAndTopologyTimeAdvanceTogether ==
    /\ csRWC.epoch = csTopologyTime
    /\ \A r \in Routers : rCachedRWC[r].epoch = rCachedTopologyTime[r]
    /\ \A s \in Shards : sCachedRWC[s].epoch = sCachedTopologyTime[s]

\* 3. Causal coupling for resolved ops: every op's resolved (rwc, topologyTime) is a
\*    value/time pair that was authoritative at the CSRS at some point. The op's
\*    topologyTime is bounded above by the current CSRS topologyTime.
ResolvedOpsAreCausallyValid ==
    \A i \in 1..Len(opLog) :
        /\ opLog[i].topologyTime <= csTopologyTime
        /\ opLog[i].rwc.epoch = opLog[i].topologyTime

\* 4. Per-router op monotonicity: ops from the same router observe non-decreasing
\*    topologyTime — a router never goes backwards in cluster time on the CWRWC plane,
\*    matching its behaviour on the routing-table plane.
PerRouterMonotonic ==
    \A r \in Routers :
        \A i, j \in 1..Len(opLog) :
            (i < j /\ opLog[i].router = r /\ opLog[j].router = r)
            => opLog[i].topologyTime <= opLog[j].topologyTime

----------------------------------------------------------------------------
\* LIVENESS PROPERTIES
\* Eventually, after the last SetDefaultRWConcern, every router and shard converges to
\* the latest value, given fair refresh.
ConvergedAtCSRS ==
    /\ \A r \in Routers : rCachedRWC[r] = csRWC
    /\ \A s \in Shards : sCachedRWC[s] = csRWC

PropertyConvergence == <>[](updateCount = MaxRWCUpdates => ConvergedAtCSRS)

----------------------------------------------------------------------------
\* STATE CONSTRAINTS (for bounded model-checking)
ConstraintBound ==
    /\ updateCount <= MaxRWCUpdates
    /\ opCount <= MaxOperations

----------------------------------------------------------------------------
\* SYMMETRY (router and shard sets are interchangeable)
Symmetry == Permutations(Routers) \cup Permutations(Shards)

============================================================================
