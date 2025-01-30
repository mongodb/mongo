\* Copyright 2025 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/
-------------------------------- MODULE TxnsCollectionIncarnation ----------------------------------
\* Formal specification for the multi-statement transactions collection incarnation consistency
\* protocol, with sub-snapshot read concern level and in the presence of DDLs.
\* It covers the `placementConflictTime' protocol that forbids the collection generation anomaly and
\* the local catalog protocol forbidding the collection incarnation anomaly described at:
\* https://github.com/mongodb/mongo/blob/master/src/mongo/db/s/README_transactions_and_ddl.md
\*
\* The model covers both TRACKED and UNTRACKED collections, and the transitions from one 
\* to the other. Explicitly covered DDLs are Create, Drop, and Rename. Reshard is implicitly covered 
\* by a back to back Drop (tracked) and Create (tracked) interleaving.
\* 
\* To run the model-checker, first edit the constants in MCTxnsCollectionIncarnation.cfg if desired, then:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh TxnsCollectionIncarnation

EXTENDS Integers, Sequences, SequencesExt, FiniteSets, TLC
CONSTANTS 
    Shards,
    NameSpaces,
    Keys,
    Txns,
    TXN_STMTS       \* statements per transaction
     
    STALE_ROUTER  == "staleRouter"
    SNAPSHOT_INCOMPATIBLE == "snapshotIncompatible"
    OK   == "ok"

    UNKNOWN == "unknown"
    TRACKED == "tracked"
    UNTRACKED == "untracked"
    INITIAL_CLUSTER_TIME == 1

\* For the spec to catch certain edge cases, we require at least 2 shards.
ASSUME Cardinality(Shards) > 1
ASSUME Cardinality(NameSpaces) > 0
ASSUME Cardinality(Txns) > 0
ASSUME TXN_STMTS \in 2..100

(* define statement *)
DroppedNamespaceUUID == 0
Stmts == 1..TXN_STMTS
Status == {SNAPSHOT_INCOMPATIBLE, STALE_ROUTER, OK}
ReqEntry == [shard:Shards, ns: NameSpaces, gen: Nat, placementConflictTs: Nat]

CreateReqEntry(s, ns, gen, placementConflictTs) == 
    [shard |->s, ns |-> ns, gen |-> gen, placementConflictTs |-> placementConflictTs]

Max(S) == CHOOSE x \in S : \A y \in S : x >= y

IsValidDataDistribution(d) ==
    /\ UNION {d[s] : s \in Shards} = Keys
    /\ \A s1, s2 \in Shards : s1 = s2 \/ d[s1] \cap d[s2] = {}
ValidDataDistributions == {d \in [Shards -> SUBSET Keys]: IsValidDataDistribution(d)}
DistributionToOwnership(d) ==  {s \in Shards : d[s] # {}}

ClusterMetadataType == {UNTRACKED, TRACKED}
\* Version generation (timestamp) 0 denotes an UNTRACKED collection.
UntrackedClusterMetadata == [type |-> UNTRACKED, gen |-> 0]

\* Tracked collection generation can never be 0.
ClusterMetadataFormat == [type: {TRACKED}, gen: Nat \ {0} ] \cup {UntrackedClusterMetadata}
CreateTrackedClusterMetadata(g) == [type |-> TRACKED, gen |-> g]

CacheMetadataFormat == [type: ClusterMetadataType \cup {UNKNOWN}, 
                        gen: Nat, 
                        ownership: SUBSET Shards]
UnknownCacheMetadata == [type |-> UNKNOWN, gen |-> 0, ownership |-> {}]

EmptyTxnResources == [snapshot |-> <<>>, locks |-> [n \in NameSpaces |-> FALSE]]

(* Global and networking variables *)
VARIABLE primaryShard       \* The primary shard for the database of untracked collections.
VARIABLE clusterMetadata    \* Authoritative metadata source.
VARIABLE clusterTime        \* The cluster time advances on DDLs.
VARIABLES log, response     \* Emulate RPC with queues between the router and the shards.
(* Router variables *)
VARIABLE rCache                 \* Per Namespace, cache of the metadata.
VARIABLE rCompletedStmt         \* Router statements acknowledged by the shard.
VARIABLE rPlacementConflictTs   \* Per-transaction, immutable timestamp that the router forwards
                                \* with each statement. Used to detect generation anomalies for 
                                \* tracked namespaces.

(* Shard variables *)
VARIABLE shardTxnResources  \* Per-shard, data+catalog snapshot and locks held for each transaction.
VARIABLE shardData          \* Per-namespace, set of keys held on each shard.
VARIABLE shardNamespaceUUID \* Per-namespace, UUID on each shard.

global_vars == << primaryShard, clusterMetadata, log, response, clusterTime >>
router_vars == << rCache, rCompletedStmt, rPlacementConflictTs>>
shard_vars == << shardTxnResources, shardData, shardNamespaceUUID >>
vars == << global_vars, router_vars, shard_vars >>

Init == (* Global and networking *)
        /\ primaryShard \in Shards
        /\ clusterMetadata = [ n \in NameSpaces |-> UntrackedClusterMetadata ]
        /\ clusterTime = INITIAL_CLUSTER_TIME
        /\ log = [ t \in Txns |-> <<>> ]
        /\ response = [ t \in Txns |-> [stm \in Stmts |-> {} ]]
        (* Router *)
        /\ rCache = [ n \in NameSpaces |-> UnknownCacheMetadata ]
        /\ rCompletedStmt = [ t \in Txns |-> 0 ]
        /\ rPlacementConflictTs = [t \in Txns |-> -1]
        (* Shard *)
        /\ shardTxnResources = [s \in Shards |-> [ t \in Txns |-> EmptyTxnResources]]
        /\ shardData = [ n \in NameSpaces |-> [ s \in Shards |-> {}]]
        /\ shardNamespaceUUID = [ n \in NameSpaces |-> [ s \in Shards |-> DroppedNamespaceUUID ]]

HasResponse(stmt) == Cardinality(stmt) # 0
TxnCommitted(t) == 
    /\  HasResponse(response[t][TXN_STMTS])
    /\  Cardinality(log[t][TXN_STMTS]) = Cardinality(response[t][TXN_STMTS])
    /\  \A rsp \in response[t][TXN_STMTS]: rsp.status = OK
TxnAborted(t) == \E s \in Stmts : HasResponse(response[t][s]) /\ \E rsp \in response[t][s]: rsp.status # OK
TxnDone(t) == TxnCommitted(t) \/ TxnAborted(t)

IsNamespaceLocked(ns) == \E s \in Shards, t \in Txns: shardTxnResources[s][t].locks[ns]

ClearResourcesForTxnInShard(t, s) == 
    [ tmpTxn \in Txns |-> IF t = tmpTxn THEN EmptyTxnResources ELSE shardTxnResources[s][tmpTxn]]
ClearResourcesForTxn(t) == [s \in Shards |-> ClearResourcesForTxnInShard(t, s)]

\* Given a Namespace -> Shard -> X
\* Return Namespace -> X where Shard = s
GetForShard(s, f) == [ns \in DOMAIN(f) |-> f[ns][s]]
CreateTxnSnapshot(s) == 
    [uuid |-> GetForShard(s, shardNamespaceUUID), data |-> GetForShard(s, shardData)]

\* Creates a cache entry for the given namespace, with the latest metadata. It is possible to cache 
\* the distribution instead of the ownership, but this would increase the state space, as many 
\* distributions map to the same ownership.
RouterCacheLookup(ns) == 
    clusterMetadata[ns] @@ [ownership |-> DistributionToOwnership(shardData[ns])]
OwnershipFromCacheEntry(cached) ==
    IF cached.type = TRACKED THEN cached.ownership ELSE {primaryShard}

TxnStmtLogEntries(t, ns) ==
    LET owningShards == OwnershipFromCacheEntry(rCache'[ns]) 
    IN  {CreateReqEntry(s, ns, rCache'[ns].gen, rPlacementConflictTs'[t]): s \in owningShards}
    
\* Action: the router forwards a transaction statement to the owning shards. If the cache entry for 
\* the namespace in the UNKNOWN state, the cache is refreshed before using it to forward statements.
RouterSendTxnStmt ( t, ns ) ==
    /\  Len(log[t]) < TXN_STMTS 
    /\  rCompletedStmt[t]=Len(log[t])
    /\  rPlacementConflictTs' = [rPlacementConflictTs EXCEPT ![t] = IF @ = -1 THEN clusterTime ELSE @]
    /\  rCache' = [rCache EXCEPT ![ns] = IF @.type = UNKNOWN THEN RouterCacheLookup(ns) ELSE @]
    /\  log' = [log EXCEPT ![t] = Append(log[t], TxnStmtLogEntries(t, ns))]
    /\  UNCHANGED << primaryShard, clusterMetadata, response, clusterTime, rCompletedStmt, shard_vars >>

\* Action: router processes a non-OK response from a shard. If the response is STALE_ROUTER, this 
\* action refreshes the cache for the stale namespace. This action implies the transaction is aborted, 
\* and is used as a shortcut to free the shards' resources for the transaction. Given that all DDLs 
\* require locks be freed on all shards, doing the freeing atomically doesn't preclude any 
\* meaningful interleaving.
RouterHandleAbort ( t, stm ) ==
    /\ Cardinality(response[t][stm])>0 
    /\ rCompletedStmt[t]<stm
    /\ \E rsp \in response[t][stm] : 
        /\ rsp.status # OK
        /\ IF rsp.status = STALE_ROUTER
            THEN rCache' = [ rCache EXCEPT ![rsp.ns] = RouterCacheLookup(rsp.ns)]
            ELSE UNCHANGED rCache
    /\ rCompletedStmt' = [rCompletedStmt EXCEPT ![t] = TXN_STMTS]
    /\ shardTxnResources' = ClearResourcesForTxn(t)
    /\ UNCHANGED << global_vars, rPlacementConflictTs, shardData, shardNamespaceUUID >>

\* Action: router processes an OK response from a shard. This action may imply the transaction
\* is committed, in which case it is used as a shortcut to free the shards' resources for the 
\* transaction. Given that all DDLs require locks be freed on all shards, doing the freeing 
\* atomically doesn't preclude any meaningful interleaving.
RouterHandleOK ( t, stm ) ==
    /\ Cardinality(response[t][stm])>0 
    /\ rCompletedStmt[t]<stm
    \* wait for as many responses OK responses as requests sent
    /\ Cardinality(log[t][stm]) = Cardinality(response[t][stm])
    /\ \E rsp \in response[t][stm] : 
        /\ rsp.status = OK
        \* /\ \* todo aggregate result
        /\ rCompletedStmt' = [rCompletedStmt EXCEPT ![t] = rCompletedStmt[t]+1]
    /\ IF TxnDone(t) 
        THEN shardTxnResources' = ClearResourcesForTxn(t) 
        ELSE UNCHANGED shardTxnResources
    /\ UNCHANGED << global_vars, rCache, rPlacementConflictTs, shardData, shardNamespaceUUID >>

GetSnapshotForNs(snap, ns) == [uuid |-> snap.uuid[ns], data|->snap.data[ns]]

ShardingMetadataCheck(currentGen, receivedGen, placementConflictTs) ==
    \* Any difference in generation should result in stale config.
    IF receivedGen # currentGen THEN STALE_ROUTER
    \* The router always forwards the latest known shard version, so even if the above check passes,
    \* it might be the case that the snapshot is no longer compatible.
    ELSE IF placementConflictTs < currentGen THEN SNAPSHOT_INCOMPATIBLE
    ELSE OK

LocalMetadataCheck(snapshotUUID, latestUUID) ==
    \* UUID == 0 has the special meaning of non-existing / dropped collection.
    \* If a collection was originally tracked, and then dropped (or recreated), an up to date router
    \* will attach the latest version (UNTRACKED) and pass the shard version check. However, the 
    \* collection in the snapshot may not necessarily be the same. As there is no way to determine 
    \* which is the correct UUID (as it is UNTRACKED), if the latest UUID is not the same as in the 
    \* snapshot, we fail the txn.
    IF snapshotUUID # latestUUID THEN SNAPSHOT_INCOMPATIBLE
    ELSE OK

\* Action: shard responds to statements written to log by router addressed to itself.
ShardResponse( self, t ) == 
    /\ LET  ln == Len(log[t]) 
            stmtReqs == log[t][ln] 
        IN
        /\ ln > 0
        \* For simplicity, snapshot cleanup is handled in the router response. Avoid re-opening the 
        \* snapshot for aborted txns.
        /\ ~TxnAborted(t)
        /\ ~\E rsp \in response[t][ln] : rsp.shard = self
        /\ \E req \in stmtReqs : 
            /\ req.shard = self
            /\ shardTxnResources' = [shardTxnResources EXCEPT 
                ![self][t].snapshot = IF DOMAIN(@) = {} THEN CreateTxnSnapshot(self) ELSE @,
                ![self][t].locks[req.ns] = TRUE]
            /\  LET txnSnapshot == shardTxnResources'[self][t].snapshot
                    shardingStatus == ShardingMetadataCheck(clusterMetadata[req.ns].gen, req.gen, req.placementConflictTs)
                    \* Only perform the local check if untracked.
                    localStatus == IF req.gen = 0 THEN LocalMetadataCheck(txnSnapshot.uuid[req.ns], shardNamespaceUUID[req.ns][self]) ELSE OK
                    rspStatus == IF shardingStatus # OK THEN shardingStatus ELSE localStatus
                IN  /\ response' = [response EXCEPT 
                        ![t][ln] = @ \union {[
                            shard|->self, 
                            ns|->req.ns, 
                            status|->rspStatus, 
                            snapshot|-> IF rspStatus # OK 
                                THEN {} 
                                ELSE GetSnapshotForNs(txnSnapshot, req.ns)
                        ]}]
    /\ UNCHANGED << primaryShard, clusterMetadata, log, clusterTime, router_vars, shardData, shardNamespaceUUID >>

NamespaceExists(ns) == \E s \in DOMAIN(shardNamespaceUUID[ns]) : shardNamespaceUUID[ns][s] # 0
NextClusterTime == clusterTime + 1

\* Action: create the namespace as untracked, the namespaces only lives in the primary shard.
CreateUntracked(ns) ==
    /\  ~IsNamespaceLocked(ns)
    /\  ~NamespaceExists(ns)
    /\  shardNamespaceUUID' = [shardNamespaceUUID EXCEPT ![ns][primaryShard] = NextClusterTime]
    /\  shardData' = [shardData EXCEPT ![ns][primaryShard] = Keys]
    /\  clusterTime' = NextClusterTime
    /\  UNCHANGED << primaryShard, clusterMetadata, log, response, router_vars, shardTxnResources >>

\* Action: create the namespace as tracked, the namespaces lives in all shards.
CreateTracked(ns, distribution) ==
    /\  ~IsNamespaceLocked(ns)
    /\  ~NamespaceExists(ns)
    /\  clusterMetadata' = [clusterMetadata EXCEPT ![ns] = CreateTrackedClusterMetadata(NextClusterTime)]
    /\  LET ownership == DistributionToOwnership(distribution)
        IN shardNamespaceUUID' = [shardNamespaceUUID EXCEPT 
            ![ns] = [s \in Shards |-> IF s \in ownership THEN NextClusterTime ELSE DroppedNamespaceUUID]]
    /\  shardData' = [shardData EXCEPT ![ns] = distribution]
    /\  clusterTime' = NextClusterTime
    /\  UNCHANGED << primaryShard, log, response, router_vars, shardTxnResources >>

DropCommon(ns, type) == 
    /\  ~IsNamespaceLocked(ns)
    /\  NamespaceExists(ns)
    /\  clusterMetadata[ns].type = type
    /\  shardNamespaceUUID' = [shardNamespaceUUID EXCEPT ![ns] = [s \in Shards |-> 0 ]]
    /\  shardData' = [shardData EXCEPT ![ns] = [s \in Shards |-> {}]]
    /\  clusterTime' = NextClusterTime
    /\  UNCHANGED << primaryShard, log, response, router_vars, shardTxnResources >>

\* Action: drop an untracked namespace.
DropUntracked(ns) ==
    /\  DropCommon(ns, UNTRACKED)
    /\  UNCHANGED << clusterMetadata >>

\* Action: drop a tracked namespace.
DropTracked(ns) ==
    /\  DropCommon(ns, TRACKED)
    /\  clusterMetadata' = [clusterMetadata EXCEPT ![ns] = UntrackedClusterMetadata ]

RenameCommon(from, to, type) ==
    /\  ~IsNamespaceLocked(from)
    /\  ~IsNamespaceLocked(to)
    /\  from # to
    /\  NamespaceExists(from)
    /\  ~NamespaceExists(to)
    /\  clusterMetadata[from].type = type
    /\  shardNamespaceUUID' = [shardNamespaceUUID EXCEPT 
            ![to] = [s \in Shards |-> shardNamespaceUUID[from][s]],
            ![from] = [s \in Shards |-> 0 ]]
    /\  shardData' = [shardData EXCEPT ![to] = shardData[from], ![from] = [s \in Shards |-> {}]]
    /\  clusterTime' = NextClusterTime
    /\  UNCHANGED << primaryShard, log, response, router_vars, shardTxnResources >>

\* Action: rename an untracked namespace. The collection's UUID is preserved.
RenameUntracked(from, to) ==
    /\  RenameCommon(from, to, UNTRACKED)
    /\  UNCHANGED << clusterMetadata >>

\* Action: rename a tracked namespace. The collection's UUID is preserved on shards, but the 
\* generation is bumped.
RenameTracked(from, to) ==
    /\  RenameCommon(from, to, TRACKED)
    /\  clusterMetadata' = [clusterMetadata EXCEPT 
        ![to] = CreateTrackedClusterMetadata(NextClusterTime),
        ![from] = UntrackedClusterMetadata ]

Next == 
    \* Router actions
    \/ \E t \in Txns, ns \in NameSpaces : RouterSendTxnStmt(t, ns)
    \/ \E t \in Txns, stm \in Stmts: RouterHandleAbort(t, stm)
    \/ \E t \in Txns, stm \in Stmts: RouterHandleOK(t, stm)
    \* Shard actions
    \/ \E s \in Shards, t \in Txns: ShardResponse( s, t )
    \* DDL actions
    \/ \E ns \in NameSpaces : CreateUntracked(ns)
    \/ \E ns \in NameSpaces, d \in ValidDataDistributions : CreateTracked(ns, d)
    \/ \E ns \in NameSpaces : DropUntracked(ns)
    \/ \E ns \in NameSpaces : DropTracked(ns)
    \/ \E from, to \in NameSpaces : RenameUntracked(from, to)
    \/ \E from, to \in NameSpaces : RenameTracked(from, to)
    \* Termination, allow infinite stuttering.
    \/ ( \A t \in Txns : rCompletedStmt[t] = TXN_STMTS /\ UNCHANGED vars )

Fairness ==
    /\ WF_vars(\E t \in Txns, ns \in NameSpaces : RouterSendTxnStmt(t, ns))
    /\ WF_vars(\E t \in Txns, stm \in Stmts: RouterHandleAbort(t, stm))
    /\ WF_vars(\E t \in Txns, stm \in Stmts: RouterHandleOK(t, stm))
    /\ WF_vars(\E s \in Shards, t \in Txns: ShardResponse( s, t ))
    /\ WF_vars(\E ns \in NameSpaces : CreateUntracked(ns))
    /\ WF_vars(\E ns \in NameSpaces, d \in ValidDataDistributions : CreateTracked(ns, d))
    /\ WF_vars(\E ns \in NameSpaces : DropUntracked(ns))
    /\ WF_vars(\E ns \in NameSpaces : DropTracked(ns))
    /\ WF_vars(\E from, to \in NameSpaces : RenameUntracked(from, to))
    /\ WF_vars(\E from, to \in NameSpaces : RenameTracked(from, to))

Spec == /\ Init /\ [][Next]_vars /\ Fairness

----------------------------------------------------------------------------------------------------

(**************************************************************************************************)
(* Type invariants.                                                                               *)
(**************************************************************************************************)

TypeOK ==
    /\  log \in [Txns -> Seq(SUBSET ReqEntry)]  
    /\  clusterMetadata \in [ NameSpaces -> ClusterMetadataFormat ]
    /\  rCache \in [ NameSpaces -> CacheMetadataFormat ]
    /\  \A ns \in NameSpaces, s \in Shards : shardData[ns][s] = {} <=> shardNamespaceUUID[ns][s] = DroppedNamespaceUUID

RouterSendsOneStmtRequestPerShard ==
    /\  \A txn \in Txns : 
        /\  \A i \in DOMAIN(log[txn]) :
            LET stmt == log[txn][i]
            IN  \* only one entry per shard
                /\  \A entry \in stmt : Len(SelectSeq(SetToSeq(stmt), LAMBDA e : e.shard = entry.shard)) = 1

(**************************************************************************************************)
(* Correctness Properties                                                                         *)
(**************************************************************************************************)

CommittedTxnImpliesAllStmtsSuccessful ==
    \A t \in Txns: TxnCommitted(t) => \A s \in Stmts : \A rsp \in response[t][s] : rsp.status = OK

UnionCompleteIntersectionNull(keySets) ==
    /\ UNION keySets = Keys
    /\ \A ks1, ks2 \in keySets : ks1 = ks2 \/ ks1 \cap ks2 = {}

StmtResponseDataCompleteAndUnique(stmtRsp) == 
    UnionCompleteIntersectionNull({rsp.snapshot.data : rsp \in stmtRsp})

CommittedTxnImpliesConsistentKeySet == \A t \in Txns: TxnCommitted(t) =>
        \A stmt \in Stmts:
            LET txnUUIDs == {rsp.snapshot.uuid : rsp \in response[t][stmt]}
                isDropUUID == \E uid \in txnUUIDs: uid = 0
            IN  \*  Check incarnation uniqueness.
                /\  Cardinality(txnUUIDs) = 1
                \*  Check data set completeness and uniqueness, or completely empty.
                /\  IF ~isDropUUID THEN
                        StmtResponseDataCompleteAndUnique(response[t][stmt])
                    ELSE
                        \A rsp \in response[t][stmt] : rsp.snapshot.data = {}

AllTxnsEventuallyDone == <>[] \A t \in Txns : TxnDone(t)
AcquiredTxnResourcesEventuallyReleased == \A s \in Shards, t \in Txns :
    shardTxnResources[s][t] # EmptyTxnResources ~> shardTxnResources[s][t] = EmptyTxnResources
    
(**************************************************************************************************)
(* Miscellaneous properties for exploring/understanding the spec.                                 *)
(**************************************************************************************************)

\* Namespaces involved in a transaction
NSsetofTxn(t) == { req.ns : req \in UNION {log[t][stmt] : stmt \in 1..Len(log[t])} }

\* Shards involved in a transaction
ShardsetofTxn(t) == { req.shard : req \in UNION {log[t][stmt] : stmt \in 1..Len(log[t])} }

\* shard-ns tuples in a transaction
SNStuplesetofTxn(t) == { <<req.shard, req.ns>> : req \in UNION {log[t][stmt] : stmt \in 1..Len(log[t])} }
====================================================================================================