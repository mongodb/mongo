\* Copyright 2025 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

--------------------------------------- MODULE MoveRange -------------------------------------------
\* This specification models the range migration commit protocol along with the routing and data
\* ownership filtering protocols for untimestamped and at a point-in-time reads.
\* The range migration commit protocol is described at
\* https://github.com/mongodb/mongo/blob/master/src/mongo/db/s/README_migrations.md
\*
\* This specification:
\* - Models ranges as single-key ranges, for simplicity. The terms 'keys' and 'ranges' are used
\*   interchangeably.
\* - Models targeted queries covering the full key space. Queries are targeted in the sense that
\*   they are routed exclusively to shards that own a range for the collection.
\* - Does not cover writes. From a routing and shard ownership perspective, writes obey the same
\*   rules as the reads modelled in this spec.
\*
\* To run the model-checker, first edit the constants in MCMoveRange.cfg if desired, then:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh MoveRange

EXTENDS Integers, Sequences, FiniteSets, TLC
CONSTANTS
    Router,
    Shards,
    Keys,
    MIGRATIONS

    staleRouter  == "staleRouter"
    ok   == "ok"

ASSUME Cardinality(Shards) > 0
ASSUME Cardinality(Keys) > 0
ASSUME Cardinality(Router) = 1
ASSUME MIGRATIONS \in 0..100

(* Global and networking variables *)
VARIABLES request, response \* Emulate RPC with queues between the router and the shards.
VARIABLE migrationState     \* Per-key migration state.
VARIABLE readTs             \* Query read timestamp. Possible values:
                            \* - UninitializedTs: uninitialized.
                            \* - [InitialTimestamp, UntimestampedRead): provided timestamp.
                            \* - UntimestampedRead: untimestamped.
VARIABLE migrations         \* Ancillary variable limiting the number of migrations.

(* Router variables *)
VARIABLE routingAttempt         \* The number of routing routingAttempts for the query.
VARIABLE rCachedRoutingTable    \* The cached routing table. Lazily maintained from `routingTable'.
VARIABLE rCachedVersions        \* The placement versions associated with the cached routing table.
                                \* Lazily maintained from `versions'.

(* Config shard variables *)
VARIABLE routingTable       \* Timestamp-aware routing table.
VARIABLE versions           \* Latest placement version associated with the routing table.

(* Shard variables *)
VARIABLE sLatestTs          \* Latest timestamp.
VARIABLE sOwnership         \* Timestamp-aware, authoritative range ownership information.
VARIABLE sVersions          \* Latest placement version associated with the ownership.
VARIABLE sKeys              \* Timestamp-aware snapshot of the keys.
VARIABLE sCritSecEnterTs    \* The timestamp at which the critical section is durably entered. The
                            \* critical section implements per-namespace prepare state, disallowing
                            \* reads. Possible values:
                            \* - UninitializedTs: no critical section.
                            \* - (InitialTimestamp, UntimestampedRead): timestamp at which the
                            \*   current critical section is entered.

(* Query-specific shard variables *)
VARIABLE sCursorTs          \* The cursor's read timestamp. See `readTs' for possible values.
VARIABLE sFilter            \* Timestamp-aware ownership filter. Used to filter out keys not owned
                            \* by the shard at a given timestamp. Also acts as a range preserver.
VARIABLE sExamined          \* Keys examined by query execution. Includes unowned keys.
VARIABLE sReturned          \* Keys returned by query execution. returned = examined ∩ filter.

vars == <<migrationState, routingAttempt, rCachedVersions, response, request, routingTable,
          rCachedRoutingTable, sVersions, sKeys, sOwnership, sCritSecEnterTs, sFilter, sCursorTs,
          migrations, sReturned, sExamined, sLatestTs, readTs, versions>>
router_vars == <<routingAttempt, rCachedVersions, rCachedRoutingTable, readTs>>
routing_table_vars == <<versions, routingTable>>
rpc_vars == <<request, response>>
shard_ownership_vars == <<sVersions, sKeys, sOwnership>>
shard_query_vars == <<sCursorTs, sFilter, sExamined, sReturned>>

Max(S) == CHOOSE x \in S : \A y \in S : x >= y

(* Timestamp definitions and operators *)
UninitializedTs == -1
InitialTs == 100
UntimestampedRead == 999999
MaxClusterTs == Max({sLatestTs[s] : s \in Shards})
TickShardTs(shard) == sLatestTs' = [sLatestTs EXCEPT ![shard] = @ + 1]
TickShardsTs(shard1, shard2) == sLatestTs' = [sLatestTs EXCEPT ![shard1] = @ + 1,
                                                               ![shard2] = @ + 1]
AdvanceTs(shard, ts) == sLatestTs' = [sLatestTs EXCEPT ![shard] = ts]

(* Routing/ownership operators *)
LatestShardOwnership(shard) == sOwnership[shard][Max(DOMAIN sOwnership[shard])]
LatestRouterCachedOwnership(router, shard) ==
    rCachedRoutingTable[router][shard][Max(DOMAIN rCachedRoutingTable[router][shard])]
LatestShardKeySet(shard) == sKeys[shard][Max(DOMAIN sKeys[shard])]

(* Migration states *)
\* Possible transitions:
\* * Unset -> Cloning -> RecipientPrepared -> AllPrepared -> ConfigShardCommitted ->
\*                                               -> RecipientCommitted -> (DonorCommitted) -> Unset
\* * Unset -> Cloning -> (Aborted) -> Unset
\* * Unset -> Cloning -> RecipientPrepared -> (Aborted) -> Unset
\* * Unset -> Cloning -> RecipientPrepared -> AllPrepared -> (Aborted) -> Unset
MigrationStateUnset == "unset"
MigrationStateCloning == "cloning"
MigrationStateRecipientPrepared == "recipientPrepared"      \* Recipient acquires critical section.
MigrationStateAllPrepared == "allPrepared"                  \* Donor acquires critical section.
MigrationStateConfigShardCommitted == "configShardCommitted"
MigrationStateRecipientCommitted == "recipientCommitted"    \* Recipient rescinds critical section.

MigrationParticipantUnset == "-"
EmptyMigrationState == [state |-> MigrationStateUnset,
                        donor |-> MigrationParticipantUnset,
                        recipient |-> MigrationParticipantUnset,
                        commitTs |-> UninitializedTs]

(* Migration operators *)
IsDonor(shard) == \E k \in Keys : migrationState[k].donor = shard
IsRecipient(shard) == \E k \in Keys : migrationState[k].recipient = shard
IsShardMigrating(shard) == IsDonor(shard) \/ IsRecipient(shard)
IsShardInCriticalSection(shard) == sCritSecEnterTs[shard] # UninitializedTs

\* A router request.
\* 's' is the target shard. 'v' is the placement version. 't' is the read timestamp.
ReqEntry == [s: Shards, v: Nat, t: Nat]

Init ==
    (* Global and networking *)
    /\ request = <<>>
    /\ response = <<>>
    /\ migrationState = [k \in Keys |-> EmptyMigrationState]        \* No ongoing migrations.
    /\ readTs = UninitializedTs
    /\ migrations = 0
    (* Shards variables *)
    /\ sLatestTs = [s \in Shards |-> InitialTs]
    /\ sKeys \in [Shards -> [{InitialTs} -> SUBSET Keys]]
    /\ \E f \in [Keys -> Shards] :                                  \* Keyset is full and unique.
        \A s \in Shards : sKeys[s][InitialTs] = {k \in Keys : f[k] = s}
    /\ sOwnership = sKeys                                           \* No orphaned keys.
    /\ sVersions \in [Shards -> 0..1]                               \* Shards owning a key have a
    /\ \A s \in Shards :                                            \* non-zero placement version,
        /\ (sOwnership[s][InitialTs] = {}) <=> (sVersions[s] = 0)   \* zero otherwise.
    /\ sCritSecEnterTs = [s \in Shards |-> UninitializedTs]         \* No critical section engaged.
    /\ sFilter = [s \in Shards |-> {}]
    /\ sReturned = [s \in Shards |-> {}]
    /\ sExamined = [s \in Shards |-> {}]
    /\ sCursorTs = [s \in Shards |-> UninitializedTs]
    (* Config shard variables *)
    /\ versions = sVersions
    /\ routingTable = sOwnership
    (* Router *)
    /\ rCachedVersions = [r \in Router |-> versions]                \* Router cache initially warm.
    /\ rCachedRoutingTable = [r \in Router |-> routingTable]        \* Router cache initially warm.
    /\ routingAttempt = 0

\* Action: the recipient shard clones a key (range) from the donor. The donor still owns the key
\* (range).
MigrateCloneRange(from, to, k) ==
    /\ migrations \in 0..MIGRATIONS-1
    /\ from # to
    /\ k \in LatestShardOwnership(from)
       \* The recipient doesn't have an orphaned range overlapping with the incoming range.
    /\ k \notin LatestShardKeySet(to)
    /\ migrationState[k].state = MigrationStateUnset
    /\ ~IsShardMigrating(from)
       \* A shard can engage in a migration if it committed a previous migration as the recipient,
       \* even if the previous migration has not yet committed on the donor.
    /\ ~IsShardMigrating(to)
    /\ TickShardTs(to)
    /\ migrationState' = [migrationState EXCEPT ![k]["state"] = MigrationStateCloning,
                                                ![k]["donor"] = from,
                                                ![k]["recipient"] = to]
    /\ sKeys' = [sKeys EXCEPT ![to] = sKeys[to] @@
                                        [x \in {sLatestTs'[to]} |-> LatestShardKeySet(to) \cup {k}]]
    /\ migrations' = migrations + 1
    /\ UNCHANGED <<router_vars, shard_query_vars, sCritSecEnterTs, rpc_vars, sVersions, sOwnership,
                   routing_table_vars>>

\*
\* Action: the recipient enters a namespace-wide prepare state.
\*
\* Note that prior to entering the prepare state, the participant drains all writes. This spec
\* glosses over write blocking because it exclusively models reads.
\*
\* In principle, the recipient shard doesn't necessarily have to enter this read-blocking prepare
\* state. In fact, some server versions implementing the weak authoritative shard model (e.g. 8.0)
\* do not. This increases the read availability, as a router that hasn't learned the new routing
\* version can continue to forward reads to a recipient that hasn't yet committed the migration;
\* only reads forwarded with the more recent version would make the shard wait until the new version
\* is installed.
\*
\* This specification introduces the critical section on the recipient as a possible implementation
\* of the strong authoritative shard model, to follow the principle that the shard should always be
\* aware of its ownership, as opposed to learning it from the outside (the router).
\*
MigrateRecipientEnterCriticalSection(s, k) ==
    /\ migrationState[k].state = MigrationStateCloning
    /\ s = migrationState[k].recipient
    /\ TickShardTs(s)
    /\ sCritSecEnterTs' = [sCritSecEnterTs EXCEPT ![s] = sLatestTs'[s]]
    /\ migrationState' = [migrationState EXCEPT ![k]["state"] = MigrationStateRecipientPrepared]
    /\ UNCHANGED <<router_vars, routing_table_vars, rpc_vars, shard_ownership_vars,
                   shard_query_vars, migrations>>

\*
\* Action: the donor enters a namespace-wide prepare state.
\*
\* Note that prior to entering the prepare state, the participant drains all writes. This spec
\* glosses over write blocking because it exclusively models reads.
\*
MigrateDonorEnterCriticalSection(s, k) ==
    /\ migrationState[k].state = MigrationStateRecipientPrepared
    /\ s = migrationState[k].donor
    /\ TickShardTs(s)
    /\ sCritSecEnterTs' = [sCritSecEnterTs EXCEPT ![s] = sLatestTs'[s]]
    /\ migrationState' = [migrationState EXCEPT ![k]["state"] = MigrationStateAllPrepared]
    /\ UNCHANGED <<router_vars, routing_table_vars, rpc_vars, shard_ownership_vars,
                   shard_query_vars, migrations>>

\*
\* Action: the config shard persists the commit decision and the global commit timestamp of the
\* migration.
\*
\* Past this stage, a router can observe a routing table that's uncommitted on the shards; the shard
\* critical sections act as a prepare conflict mechanism to present a consistent view of the
\* ownership.
\*
MigrateCommitOnConfigShard(k) ==
    /\ migrationState[k].state = MigrationStateAllPrepared
    /\ LET from == migrationState[k].donor
           to   == migrationState[k].recipient
           ms   == migrationState
           rt   == routingTable
           v    == versions IN
        /\ migrationState' = [ms EXCEPT ![k]["state"] = MigrationStateConfigShardCommitted,
                                        ![k]["commitTs"] =
                                                        Max({sLatestTs[from], sLatestTs[to]}) + 1]
        /\ routingTable' = [rt EXCEPT ![from] = rt[from] @@
                                [x \in {ms'[k]["commitTs"]} |-> LatestShardOwnership(from) \ {k}],
                                       ![to] = rt[to] @@
                                [x \in {ms'[k]["commitTs"]} |-> LatestShardOwnership(to) \cup {k}]]
        /\ versions' = [v EXCEPT ![from] = IF rt'[from][Max(DOMAIN rt'[from])] = {}
                                                THEN 0
                                                ELSE Max({v[x] : x \in Shards}) + 1,
                                 ![to] = Max({v[x] : x \in Shards}) + 1]
    /\ UNCHANGED <<router_vars, shard_query_vars, rpc_vars, shard_ownership_vars, sCritSecEnterTs,
                   migrations, sLatestTs>>

\* Action: the recipient shard commits the ownership change.
MigrateCommitOnRecipientShard(s, k) ==
    /\ migrationState[k].state = MigrationStateConfigShardCommitted
    /\ migrationState[k].recipient = s
    /\ AdvanceTs(s, migrationState[k]["commitTs"])
    /\ sOwnership' = [sOwnership EXCEPT ![s] = sOwnership[s] @@
                                    [x \in {sLatestTs'[s]} |-> LatestShardOwnership(s) \cup {k}]]
    /\ sCritSecEnterTs' = [sCritSecEnterTs EXCEPT ![s] = UninitializedTs]
    /\ migrationState' = [migrationState EXCEPT ![k]["state"] = MigrationStateRecipientCommitted,
                                                ![k]["recipient"] = MigrationParticipantUnset]
    /\ sVersions' = [sVersions EXCEPT ![s] = versions[s]]
    /\ UNCHANGED <<router_vars, routing_table_vars, rpc_vars, shard_query_vars, sKeys, migrations>>

\* Action: the donor shard commits the ownership change.
MigrateCommitOnDonorShard(s, k) ==
    /\ migrationState[k].state = MigrationStateRecipientCommitted
    /\ s = migrationState[k].donor
    /\ AdvanceTs(s, migrationState[k]["commitTs"])
    /\ sOwnership' = [sOwnership EXCEPT ![s] =
                        sOwnership[s] @@ [x \in {sLatestTs'[s]} |-> LatestShardOwnership(s) \ {k}]]
    /\ sCritSecEnterTs' = [sCritSecEnterTs EXCEPT ![s] = UninitializedTs]
    /\ migrationState' = [migrationState EXCEPT ![k] = EmptyMigrationState]
    /\ sVersions' = [sVersions EXCEPT ![s] = versions[s]]
    /\ UNCHANGED <<router_vars, routing_table_vars, rpc_vars, shard_query_vars, sKeys, migrations>>

\* Action: abort an ongoing range migration.
\*
\* The migration is abortable until the coordinator (the config shard) persists the commit decision
\* via the `MigrateCommitOnConfigShard' action.
\*
MigrateAbort(k) ==
    /\ migrationState[k].state \in {MigrationStateCloning, MigrationStateRecipientPrepared,
                                    MigrationStateAllPrepared}
    /\ LET from == migrationState[k].donor
           to   == migrationState[k].recipient IN
        /\ sCritSecEnterTs' = [sCritSecEnterTs EXCEPT ![from] = UninitializedTs,
                                                      ![to] = UninitializedTs]
           \* Tick the timestamps of the participant shards releasing the critical section, if any.
        /\ IF migrationState[k].state = MigrationStateCloning
            THEN
                /\ UNCHANGED sLatestTs
            ELSE IF migrationState[k].state = MigrationStateRecipientPrepared
                THEN
                    /\ TickShardTs(to)
            ELSE \* MigrationStateAllPrepared
                /\ TickShardsTs(from, to)
    /\ migrationState' = [migrationState EXCEPT ![k] = EmptyMigrationState]
    /\ UNCHANGED <<router_vars, routing_table_vars, rpc_vars, shard_query_vars,
                   shard_ownership_vars, migrations>>

\* Action: clean up a key (range) that is not owned by the shard, provided the key (range) is not
\* undergoing migration or is not pinned by the range preserver.
DeleteRange(s, k) ==
    /\ migrationState[k].state = MigrationStateUnset
    /\ k \in LatestShardKeySet(s)
    /\ k \notin LatestShardOwnership(s)
    /\ k \notin sFilter[s] \* The range is not pinned by the range preserver.
    /\ TickShardTs(s)
    /\ sKeys' = [sKeys EXCEPT ![s] =
                                sKeys[s] @@ [x \in {sLatestTs'[s]} |-> LatestShardKeySet(s) \ {k}]]
    /\ UNCHANGED <<router_vars, routing_table_vars, rpc_vars, shard_query_vars, sCritSecEnterTs,
                   migrationState, sVersions, sOwnership, migrations>>

\* Action: forward a request to the owning shards that covers the entire key space.
Route(r, t) ==
    /\ Len(request) = 0
    /\ routingAttempt' = 1
    /\ readTs' = t
    /\ LET fanout == {s \in DOMAIN rCachedRoutingTable[r] : rCachedRoutingTable[r][s]
                            [Max({x \in DOMAIN rCachedRoutingTable[r][s] : x <= readTs'})] # {}} IN
        /\ request' = Append(request,
                                 {[s |-> s, v |-> rCachedVersions[r][s], t |-> t] : s \in fanout})
    /\ response' = Append(response, {})
    /\ UNCHANGED <<routing_table_vars, shard_ownership_vars, shard_query_vars, sCritSecEnterTs,
                   migrationState, rCachedRoutingTable, rCachedVersions, migrations, sLatestTs>>

\* Action: retry a failed request.
RouteRetry(r) ==
    /\ Len(request) # 0 /\ \E rsp \in response[routingAttempt] : rsp.rsp # ok
    /\ rCachedVersions' = [rt \in Router |-> versions]
    /\ rCachedRoutingTable' = [rt \in Router |-> routingTable]
    /\ routingAttempt' = routingAttempt + 1
    /\ LET fanout == {s \in DOMAIN rCachedRoutingTable'[r] : rCachedRoutingTable'[r][s]
                            [Max({t \in DOMAIN rCachedRoutingTable'[r][s] : t <= readTs})] # {}} IN
        /\ request' = Append(request, {[s |-> s, v |-> rCachedVersions'[r][s], t |-> readTs] :
                                                                                    s \in fanout})
    /\ response' = Append(response, {})
    \* Prime the shards query state, to avoid increasing the state space unnecessarily.
    /\ sCursorTs' = [s \in Shards |-> UninitializedTs]
    /\ sFilter' = [s \in Shards |-> {}]
    /\ sReturned' = [s \in Shards |-> {}]
    /\ sExamined' = [s \in Shards |-> {}]
    /\ UNCHANGED <<routing_table_vars, shard_ownership_vars, sCritSecEnterTs, migrationState,
                   migrations, sLatestTs, readTs>>

\*
\* Action: establish a cursor on the shard and install the appropriate ownership filter.
\*
\* Untimestamped reads install an ownership filter reflective of the latest data distribution.
\* Timestamped reads install the ownership filter at the provided point in time.
\*
ShardEstablishCursor(s) ==
    /\ routingAttempt > 0
    /\ \E stmt \in request[routingAttempt] : stmt.s = s
    /\ ~\E rsp \in response[routingAttempt] : rsp.s = s
    /\ sCursorTs[s] = UninitializedTs
    /\ ~IsShardInCriticalSection(s)
    /\ LET req == CHOOSE stmt \in request[routingAttempt] : stmt.s = s
           rTs == req.t
           receivedVersion == req.v IN
            /\ IF receivedVersion # sVersions[s] THEN
                /\ response' = [response EXCEPT ![routingAttempt] =
                                                        @ \cup {[s |-> s, rsp |-> staleRouter]}]
                /\ UNCHANGED <<sCursorTs, sFilter, sLatestTs>>
               ELSE
                /\ response' = [response EXCEPT ![routingAttempt] = @ \cup {[s |-> s, rsp |-> ok]}]
                /\ sCursorTs' = [sCursorTs EXCEPT ![s] = rTs]
                /\ sFilter' = [sFilter EXCEPT ![s] =
                            sOwnership[s][Max({t \in DOMAIN sOwnership[s] : t <= sCursorTs'[s]})]]
    /\ UNCHANGED <<router_vars, routing_table_vars, shard_ownership_vars, sCritSecEnterTs,
                   sReturned, sExamined, migrationState, request, migrations, sLatestTs>>

\*
\* Action: the shard examines a key in its data snapshot, and returns the key if it intersects the
\* ownership filter installed when the cursor was established.
\*
\* For untimestamped reads, the data snapshot advances freely.
\*
ShardExamineKey(s, k) ==
    /\ sCursorTs[s] # UninitializedTs
    /\ k \in sKeys[s][Max({t \in DOMAIN sKeys[s] : t <= sCursorTs[s]})]
    /\ k \notin sExamined[s]
    /\ ~IsShardInCriticalSection(s)
    /\ sExamined' = [sExamined EXCEPT ![s] = @ \cup {k}]
    /\ IF k \in sFilter[s]
        THEN sReturned' = [sReturned EXCEPT ![s] = @ \cup {k}]
        ELSE UNCHANGED <<sReturned>>
    /\ UNCHANGED <<router_vars, routing_table_vars, rpc_vars, shard_ownership_vars, sCritSecEnterTs,
                   migrationState, migrations, sCursorTs, sFilter, sLatestTs>>

Next ==
    \/ \E k \in Keys, from \in Shards, to \in Shards : MigrateCloneRange(from, to, k)
    \/ \E s \in Shards, k \in Keys : MigrateRecipientEnterCriticalSection(s, k)
    \/ \E s \in Shards, k \in Keys : MigrateDonorEnterCriticalSection(s, k)
    \/ \E k \in Keys : MigrateCommitOnConfigShard(k)
    \/ \E s \in Shards, k \in Keys : MigrateCommitOnRecipientShard(s, k)
    \/ \E s \in Shards, k \in Keys : MigrateCommitOnDonorShard(s, k)
    \/ \E k \in Keys : MigrateAbort(k)
    \/ \E s \in Shards, k \in Keys : DeleteRange(s, k)
    \/ \E r \in Router, t \in InitialTs..MaxClusterTs \cup UntimestampedRead..UntimestampedRead :
                                                                                        Route(r, t)
    \/ \E r \in Router : RouteRetry(r)
    \/ \E s \in Shards : ShardEstablishCursor(s)
    \/ \E k \in Keys, s \in Shards : ShardExamineKey(s, k)

Fairness == TRUE
    /\ WF_vars(\E k \in Keys, from \in Shards, to \in Shards : MigrateCloneRange(from, to, k))
    /\ WF_vars(\E s \in Shards, k \in Keys : MigrateDonorEnterCriticalSection(s, k))
    /\ WF_vars(\E k \in Keys : MigrateCommitOnConfigShard(k))
    /\ WF_vars(\E s \in Shards, k \in Keys : MigrateCommitOnRecipientShard(s, k))
    /\ WF_vars(\E s \in Shards, k \in Keys : MigrateCommitOnDonorShard(s, k))
    /\ WF_vars(\E k \in Keys : MigrateAbort(k))
    /\ WF_vars(\E s \in Shards, k \in Keys : DeleteRange(s, k))
    /\ WF_vars(\E r \in Router, t \in InitialTs..MaxClusterTs \cup
                                                            UntimestampedRead..UntimestampedRead :
                                                                                        Route(r, t))
    /\ WF_vars(\E r \in Router : RouteRetry(r))
    /\ WF_vars(\E s \in Shards: ShardEstablishCursor(s))
    /\ WF_vars(\E k \in Keys, s \in Shards : ShardExamineKey(s, k))

Spec == /\ Init /\ [][Next]_vars /\ Fairness

----------------------------------------------------------------------------------------------------
(**************************************************************************************************)
(* Type invariants                                                                                *)
(**************************************************************************************************)

TypeOK ==
    /\ request \in Seq(SUBSET ReqEntry)
      \* No overlap between commit timestamps and the virtual timestamp representing untimestamped
      \* reads.
    /\ \A s \in Shards : sLatestTs[s] \in InitialTs..UntimestampedRead-1
    /\ \A s \in Shards : sCritSecEnterTs[s] \in
                            (UninitializedTs..UninitializedTs \cup InitialTs..UntimestampedRead-1)
    /\ readTs \in UninitializedTs..UninitializedTs \cup InitialTs..UntimestampedRead

(**************************************************************************************************)
(* Correctness Properties                                                                         *)
(**************************************************************************************************)

\* The query doesn't return duplicate keys.
UniqueKeysReturned == \A s1, s2 \in Shards : s1 = s2 \/ sReturned[s1] \cap sReturned[s2] = {}

\* Untimestamped reads exclusively target shards that own a range.
RoutingUntimestampedReads ==
    /\ \A req \in DOMAIN request: \A r \in request[req] :
        \/ r.v # 0
        \/ r.t < UntimestampedRead  \* Timestamped read.

\* The routing table has no overlapping ranges.
RoutingTableConsistent ==
    /\ LET rt == routingTable IN
        /\ \A s1, s2 \in Shards : s1 = s2
            \/ rt[s1][Max(DOMAIN rt[s1])] \cap rt[s2][Max(DOMAIN rt[s2])] = {}

\* The shards' range ownership information has no overlapping ranges, outside of the prepare state.
ShardOwnershipConsistent ==
    /\ LET o == sOwnership IN
        /\ \A s1, s2 \in Shards :
            \/ s1 = s2
            \/ IsShardInCriticalSection(s1)
            \/ IsShardInCriticalSection(s2)
            \/ o[s1][Max(DOMAIN o[s1])] \cap o[s2][Max(DOMAIN o[s2])] = {}

\* Shards owning a range at the latest timestamp must have a non-zero placement version.
OwningShardsHaveNonZeroPlacementVersion ==
    /\ \A s \in Shards : LatestShardOwnership(s) = {} \/ sVersions[s] # 0

\* Shards not owning a range at the latest timestamp must have a placement version set to zero.
NotOwningShardsHaveZeroPlacementVersion ==
    /\ \A s \in Shards : LatestShardOwnership(s) # {} \/ sVersions[s] = 0

(**************************************************************************************************)
(* Liveness properties                                                                            *)
(**************************************************************************************************)

\* The query doesn't miss a key.
PropertyAllKeysReturned == <>(UNION {sReturned[s] : s \in DOMAIN sReturned} = Keys)
====================================================================================================
