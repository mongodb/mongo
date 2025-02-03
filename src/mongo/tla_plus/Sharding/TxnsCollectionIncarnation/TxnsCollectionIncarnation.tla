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
\* The specification models a single database with both TRACKED and UNTRACKED collections, and the 
\* transitions from one to the other. Explicitly covered DDLs are Create, Drop, Rename, and 
\* MovePrimary. Reshard is implicitly covered  by a back to back Drop (tracked) and Create (tracked)
\* interleaving.
\*
\* A noteworthy fact is that movePrimary differs from the actual implementation in that database locks
\* are not specified. This allows movePrimary to happen with a transaction active on the namespaces in
\* the database. The upside of this is that the model is kept simpler while allowing exploration of 
\* states equivalent to those that can happen in reality, but in practice would require 2 or more 
\* databases to cause an anomaly (e.g. SERVER-82353).
\*
\* The ShardVersion is only partially modelled to consider the timestamp field, referred to as 
\* 'collectionGen' in this spec. Similarly the DatabaseVersion only considers the timestamp field, 
\* and is referred to as 'dbVersion'.
\* 
\* To run the model-checker, first edit the constants in MCTxnsCollectionIncarnation.cfg if desired, then:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh TxnsCollectionIncarnation

EXTENDS Integers, Sequences, FiniteSets, TLC
CONSTANTS 
    Shards,
    NameSpaces,
    Keys,
    Txns,
    TXN_STMTS       \* statements per transaction
    
    STALE_DB_VERSION  == "staleDbVersion"
    STALE_SHARD_VERSION  == "staleShardVersion"
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
Max(S) == CHOOSE x \in S : \A y \in S : x >= y
IsInjective(f) == \A a,b \in DOMAIN f : f[a] = f[b] => a = b

DroppedNamespaceUUID == 0
Stmts == 1..TXN_STMTS
ReqEntry == [shard: Shards, ns: NameSpaces, dbVersion: Nat, collectionGen: Nat, placementConflictTs: Nat]

CreateReqEntry(s, ns, dbVersion, collectionGen, placementConflictTs) == 
    [shard |->s, ns |-> ns, dbVersion |-> dbVersion, collectionGen |-> collectionGen, placementConflictTs |-> placementConflictTs]

IsValidDataDistribution(d) ==
    /\ UNION {d[s] : s \in Shards} = Keys
    /\ \A s1, s2 \in Shards : s1 = s2 \/ d[s1] \cap d[s2] = {}
ValidDataDistributions == {d \in [Shards -> SUBSET Keys]: IsValidDataDistribution(d)}
DistributionToOwnership(d) ==  {s \in Shards : d[s] # {}}

UntrackedCollectionGen == 0
CollectionMetadataType == {UNTRACKED, TRACKED}
\* Version generation (timestamp) 0 denotes an UNTRACKED collection.
UntrackedClusterMetadata == [type |-> UNTRACKED, collectionGen |-> UntrackedCollectionGen]

CollectionMetadataFormat == [type: {TRACKED}, collectionGen: Nat \ {UntrackedCollectionGen} ] \cup {UntrackedClusterMetadata}
CreateTrackedCollectionMetadata(g) == [type |-> TRACKED, collectionGen |-> g]

CollectionCacheMetadataFormat == 
    [ type: CollectionMetadataType \cup {UNKNOWN}, collectionGen: Nat, ownership: SUBSET Shards ]

UnknownCollectionCacheMetadata == [type |-> UNKNOWN, collectionGen |-> UntrackedCollectionGen, ownership |-> {}]

EmptyTxnResources == [snapshot |-> <<>>, locks |-> [n \in NameSpaces |-> FALSE]]

NoDatabaseVersion == 0
DatabaseMetadataFormat == [primaryShard: Shards, dbVersion: Nat]

(* Global and networking variables *)
VARIABLE databaseMetadata   \* Authoritative metadata source for the database (single).
VARIABLE collectionMetadata \* Authoritative metadata source for collections.
VARIABLE clusterTime        \* The cluster time advances on DDLs.
VARIABLES log, response     \* Emulate RPC with queues between the router and the shards.
VARIABLE nextUUID           \* Auxiliary variable to simulate UUID generation.

(* Router variables *)
VARIABLE rDatabaseCache         \* Cache of the database metadata.
VARIABLE rCollectionCache       \* Per Namespace, cache of the metadata.
VARIABLE rCompletedStmt         \* Router statements acknowledged by the shard.
VARIABLE rPlacementConflictTs   \* Per-transaction, immutable timestamp that the router forwards
                                \* with each statement. Used to detect generation anomalies for 
                                \* tracked namespaces.

(* Shard variables *)
VARIABLE shardTxnResources  \* Per-shard, data+catalog snapshot and locks held for each transaction.
VARIABLE shardData          \* Per-namespace, set of keys held on each shard.
VARIABLE shardNamespaceUUID \* Per-namespace, UUID on each shard.

global_vars == << databaseMetadata, collectionMetadata, log, response, clusterTime, nextUUID >>
router_vars == << rDatabaseCache, rCollectionCache, rCompletedStmt, rPlacementConflictTs >>
shard_vars == << shardTxnResources, shardData, shardNamespaceUUID >>
vars == << global_vars, router_vars, shard_vars >>

Init == (* Global and networking *)
        /\ databaseMetadata \in [primaryShard: Shards, dbVersion: {INITIAL_CLUSTER_TIME}]
        /\ collectionMetadata = [ n \in NameSpaces |-> UntrackedClusterMetadata ]
        /\ clusterTime = INITIAL_CLUSTER_TIME
        /\ log = [ t \in Txns |-> <<>> ]
        /\ response = [ t \in Txns |-> [stm \in Stmts |-> {} ]]
        /\ nextUUID = 1000
        (* Router *)
        /\ rDatabaseCache = databaseMetadata
        /\ rCollectionCache = [ n \in NameSpaces |-> UnknownCollectionCacheMetadata ]
        /\ rCompletedStmt = [ t \in Txns |-> 0 ]
        /\ rPlacementConflictTs = [t \in Txns |-> -1]
        (* Shard *)
        /\ shardTxnResources = [s \in Shards |-> [ t \in Txns |-> EmptyTxnResources]]
        /\ shardData = [ n \in NameSpaces |-> [ s \in Shards |-> {}]]
        /\ shardNamespaceUUID = [ x \in {clusterTime} |-> [ n \in NameSpaces |-> [ s \in Shards |-> DroppedNamespaceUUID ]]]

LatestShardNamespaceUUID == shardNamespaceUUID[Max(DOMAIN shardNamespaceUUID)]
LatestUUIDForNameSpace(ns) == Max({LatestShardNamespaceUUID[ns][s] : s \in Shards})

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
CreateTxnSnapshot(s) == LET ts == Max(DOMAIN shardNamespaceUUID) IN
    [   ts |-> ts,
        uuid |-> GetForShard(s, shardNamespaceUUID[clusterTime]), 
        data |-> GetForShard(s, shardData)  ]

\* Creates a cache entry for the given namespace, with the latest metadata. It is possible to cache 
\* the distribution instead of the ownership, but this would increase the state space, as many 
\* distributions map to the same ownership.
RouterCacheLookup(ns) == 
    collectionMetadata[ns] @@ [ownership |-> DistributionToOwnership(shardData[ns])]
OwnershipFromCacheEntry(cached) ==
    IF cached.type = TRACKED THEN cached.ownership ELSE {rDatabaseCache.primaryShard}

TxnStmtLogEntries(t, ns) ==
    LET owningShards == OwnershipFromCacheEntry(rCollectionCache'[ns])
        collectionGen == rCollectionCache'[ns].collectionGen
        dbVersion == IF collectionGen # UntrackedCollectionGen THEN NoDatabaseVersion ELSE rDatabaseCache.dbVersion 
    IN  {CreateReqEntry(s, ns, dbVersion, collectionGen, rPlacementConflictTs'[t]): s \in owningShards}
    
\* Action: the router forwards a transaction statement to the owning shards. If the cache entry for 
\* the namespace in the UNKNOWN state, the cache is refreshed before using it to forward statements.
RouterSendTxnStmt ( t, ns ) ==
    /\  Len(log[t]) < TXN_STMTS 
    /\  rCompletedStmt[t]=Len(log[t])
    /\  rPlacementConflictTs' = [rPlacementConflictTs EXCEPT ![t] = IF @ = -1 THEN clusterTime ELSE @]
    /\  rCollectionCache' = [rCollectionCache EXCEPT ![ns] = IF @.type = UNKNOWN THEN RouterCacheLookup(ns) ELSE @]
    /\  log' = [log EXCEPT ![t] = Append(log[t], TxnStmtLogEntries(t, ns))]
    /\  UNCHANGED << databaseMetadata, collectionMetadata, response, clusterTime, nextUUID,
            rDatabaseCache, rCompletedStmt, shard_vars >>

\* Action: router processes a non-OK response from a shard. If the response is STALE_SHARD_VERSION 
\* or STALE_DB_VERSION, this action refreshes the cache for the stale collection or database. This 
\* action implies the transaction is aborted, and is used as a shortcut to free the shards' resources 
\* for the transaction. Given that all DDLs require locks be freed on all shards, doing the freeing 
\* atomically doesn't preclude any meaningful interleaving.
RouterHandleAbort ( t, stm ) ==
    /\ Cardinality(response[t][stm])>0 
    /\ rCompletedStmt[t]<stm
    /\ \E rsp \in response[t][stm] : 
        /\ rsp.status # OK
        /\  \/  /\ rsp.status = STALE_SHARD_VERSION
                /\ rCollectionCache' = [ rCollectionCache EXCEPT ![rsp.ns] = RouterCacheLookup(rsp.ns)]
                /\ UNCHANGED rDatabaseCache
            \/  /\ rsp.status = STALE_DB_VERSION
                /\ rDatabaseCache' = databaseMetadata
                /\ UNCHANGED rCollectionCache
            \/  UNCHANGED << rDatabaseCache, rCollectionCache >>
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
    /\ UNCHANGED << global_vars, rDatabaseCache, rCollectionCache, rPlacementConflictTs, shardData, 
        shardNamespaceUUID >>

GetSnapshotForNs(snap, ns) == [uuid |-> snap.uuid[ns], data|->snap.data[ns]]

DatabaseMetadataCheck(self, req) ==
    IF req.dbVersion # databaseMetadata.dbVersion THEN STALE_DB_VERSION
    ELSE IF req.placementConflictTs < databaseMetadata.dbVersion THEN SNAPSHOT_INCOMPATIBLE
    ELSE OK

ShardingMetadataCheck(req) ==
    LET receivedGen == req.collectionGen
        currentGen == collectionMetadata[req.ns].collectionGen
        placementConflictTs == req.placementConflictTs IN
    \* Any difference in generation should result in stale config.
    IF receivedGen # currentGen THEN STALE_SHARD_VERSION
    \* The router always forwards the latest known shard version, so even if the above check passes,
    \* it might be the case that the snapshot is no longer compatible.
    ELSE IF placementConflictTs < currentGen THEN SNAPSHOT_INCOMPATIBLE
    ELSE OK

LocalMetadataCheck(self, req, txnSnapshot) ==
    LET snapshotUUID == txnSnapshot.uuid[req.ns]
        latestUUID == LatestShardNamespaceUUID[req.ns][self] IN
    \* UUID == 0 has the special meaning of non-existing / dropped collection.
    \* If a collection was originally tracked, and then dropped (or recreated), an up to date router
    \* will attach the latest version (UNTRACKED) and pass the shard version check. However, the 
    \* collection in the snapshot may not necessarily be the same. As there is no way to determine 
    \* which is the correct UUID (as it is UNTRACKED), if the latest UUID is not the same as in the 
    \* snapshot, we fail the txn.
    IF snapshotUUID # latestUUID THEN SNAPSHOT_INCOMPATIBLE
    ELSE OK

ResponseFromSnapshot(self, ns, status, txnSnapshot) ==
    [   shard |-> self, 
        ns |-> ns, 
        status |-> status, 
        snapshot |-> IF status # OK THEN {} ELSE GetSnapshotForNs(txnSnapshot, ns) ]

MetadataCheck(self, t, req, txnSnapshot) ==
    LET 
        databaseVersionStatus == DatabaseMetadataCheck(self, req)
        shardVersionStatus == ShardingMetadataCheck(req)
        localStatus == LocalMetadataCheck(self, req, txnSnapshot) 
    IN 
    \* Shard version attached, only check the shard version.
    IF req.collectionGen # UntrackedCollectionGen THEN shardVersionStatus
    ELSE 
        IF databaseVersionStatus # OK THEN databaseVersionStatus
        ELSE IF shardVersionStatus # OK THEN shardVersionStatus
        ELSE localStatus

\* Action: shard responds to statements written to log by router addressed to itself.
ShardResponse( self, t ) == 
    /\  LET stmt == Len(log[t]) 
            stmtReqs == log[t][stmt] 
        IN
        /\ stmt > 0
        \* For simplicity, snapshot cleanup is handled in the router response. Avoid re-opening the 
        \* snapshot for aborted txns.
        /\ ~TxnAborted(t)
        /\ ~\E rsp \in response[t][stmt] : rsp.shard = self
        /\ \E req \in stmtReqs : 
            /\ req.shard = self
            /\ shardTxnResources' = [shardTxnResources EXCEPT 
                ![self][t].snapshot = IF DOMAIN(@) = {} THEN CreateTxnSnapshot(self) ELSE @,
                ![self][t].locks[req.ns] = TRUE]
            /\  LET txnSnapshot == shardTxnResources'[self][t].snapshot
                    rspStatus == MetadataCheck(self, t, req, txnSnapshot)
                IN  /\ response' = [response EXCEPT ![t][stmt] = @ \union {ResponseFromSnapshot(self, req.ns, rspStatus, txnSnapshot)}]
    /\ UNCHANGED << databaseMetadata, collectionMetadata, log, clusterTime, nextUUID, router_vars, 
        shardData, shardNamespaceUUID >>

NamespaceExists(ns) == \E s \in DOMAIN(LatestShardNamespaceUUID[ns]) : LatestShardNamespaceUUID[ns][s] # DroppedNamespaceUUID
NextClusterTime == clusterTime + 1

\* Action: create the namespace as untracked, the namespaces only lives in the primary shard.
CreateUntracked(ns) ==
    /\  ~IsNamespaceLocked(ns)
    /\  ~NamespaceExists(ns)
    /\  shardNamespaceUUID' = shardNamespaceUUID @@ [x \in {NextClusterTime} |-> [LatestShardNamespaceUUID EXCEPT ![ns][databaseMetadata.primaryShard] = nextUUID]]
    /\  shardData' = [shardData EXCEPT ![ns][databaseMetadata.primaryShard] = Keys]
    /\  clusterTime' = NextClusterTime
    /\  nextUUID' = nextUUID + 1
    /\  UNCHANGED << databaseMetadata, collectionMetadata, log, response, router_vars, shardTxnResources >>

\* Action: create the namespace as tracked, the namespaces exists in owning shards + primary shard.
\* Data can be distributed arbitrarily. The primary shard always having the collection, even if empty, 
\* ensures the local catalog protocol forbids the collection incarnation anomaly in the case a 
\* tracked collection is dropped, and a request for the now untracked namespace is forwarded to the 
\* primary shard, but the transaction snapshot was established at a time before the drop.
CreateTracked(ns, distribution) ==
    /\  ~IsNamespaceLocked(ns)
    /\  ~NamespaceExists(ns)
    /\  collectionMetadata' = [collectionMetadata EXCEPT ![ns] = CreateTrackedCollectionMetadata(NextClusterTime)]
    /\  LET ownership == DistributionToOwnership(distribution) \cup {databaseMetadata.primaryShard}
        IN shardNamespaceUUID' = shardNamespaceUUID @@ [x \in {NextClusterTime} |-> [LatestShardNamespaceUUID EXCEPT 
            ![ns] = [s \in Shards |-> IF s \in ownership THEN nextUUID ELSE DroppedNamespaceUUID]]]
    /\  shardData' = [shardData EXCEPT ![ns] = distribution]
    /\  clusterTime' = NextClusterTime
    /\  nextUUID' = nextUUID + 1
    /\  UNCHANGED << databaseMetadata, log, response, router_vars, shardTxnResources >>

DropCommon(ns, type) == 
    /\  ~IsNamespaceLocked(ns)
    /\  NamespaceExists(ns)
    /\  collectionMetadata[ns].type = type
    /\  shardNamespaceUUID' = shardNamespaceUUID @@ [x \in {NextClusterTime} |-> [LatestShardNamespaceUUID EXCEPT ![ns] = [s \in Shards |-> DroppedNamespaceUUID ]]]
    /\  shardData' = [shardData EXCEPT ![ns] = [s \in Shards |-> {}]]
    /\  clusterTime' = NextClusterTime
    /\  UNCHANGED << databaseMetadata, log, response, nextUUID, router_vars, shardTxnResources >>

\* Action: drop an untracked namespace.
DropUntracked(ns) ==
    /\  DropCommon(ns, UNTRACKED)
    /\  UNCHANGED << collectionMetadata >>

\* Action: drop a tracked namespace.
DropTracked(ns) ==
    /\  DropCommon(ns, TRACKED)
    /\  collectionMetadata' = [collectionMetadata EXCEPT ![ns] = UntrackedClusterMetadata ]

RenameCommon(from, to, type) ==
    /\  ~IsNamespaceLocked(from)
    /\  ~IsNamespaceLocked(to)
    /\  from # to
    /\  NamespaceExists(from)
    /\  ~NamespaceExists(to)
    /\  collectionMetadata[from].type = type
    /\  shardNamespaceUUID' = shardNamespaceUUID @@ [x \in {NextClusterTime} |-> 
            [LatestShardNamespaceUUID EXCEPT 
                ![to] = [s \in Shards |-> LatestShardNamespaceUUID[from][s]],
                ![from] = [s \in Shards |-> DroppedNamespaceUUID ]]
        ]
    /\  shardData' = [shardData EXCEPT ![to] = shardData[from], ![from] = [s \in Shards |-> {}]]
    /\  clusterTime' = NextClusterTime
    /\  UNCHANGED << databaseMetadata, log, response, nextUUID, router_vars, shardTxnResources >>

\* Action: rename an untracked namespace. The collection's UUID is preserved.
RenameUntracked(from, to) ==
    /\  RenameCommon(from, to, UNTRACKED)
    /\  UNCHANGED << collectionMetadata >>

\* Action: rename a tracked namespace. The collection's UUID is preserved on shards, but the 
\* generation is bumped.
RenameTracked(from, to) ==
    /\  RenameCommon(from, to, TRACKED)
    /\  collectionMetadata' = [collectionMetadata EXCEPT 
        ![to] = CreateTrackedCollectionMetadata(NextClusterTime),
        ![from] = UntrackedClusterMetadata ]

IsUntrackedAndExists(ns) ==
    /\ collectionMetadata[ns].type = UNTRACKED
    /\ LatestShardNamespaceUUID[ns][databaseMetadata.primaryShard] # DroppedNamespaceUUID
    
\* Action: move database to another primary shard. All UNTRACKED collections are migrated to the new
\* primary shard, and the collections' UUIDs are changed. Tracked collections always exist on the 
\* primary shard, if it doesn't exist at the destination, clones the collection metadata.
MovePrimary(toShard) ==
    LET nsToMove == {ns \in NameSpaces: IsUntrackedAndExists(ns)}
        fromShard == databaseMetadata.primaryShard
    IN
    /\  toShard # fromShard
    /\  ~\E ns \in nsToMove: IsNamespaceLocked(ns)
    /\  LET uuidSet == nextUUID..(nextUUID + Cardinality(nsToMove))
            newUUIDForNs == CHOOSE s \in [nsToMove -> uuidSet]: IsInjective(s)
        IN 
        /\ shardNamespaceUUID' = shardNamespaceUUID @@ [x \in {NextClusterTime} |-> 
            [ ns \in NameSpaces |-> IF ns \in nsToMove
                \* UNTRACKED collections, collection is created at destination and removed from source.
                THEN [ s \in Shards |-> IF s = toShard THEN newUUIDForNs[ns] ELSE DroppedNamespaceUUID ]
                \* TRACKED collections: if necessary, collection is created at destination as empty, 
                \* but it is not dropped from source even if no chunks were owned. This is to
                \* prevent ongoing queries from being unnecessarily killed, and to preserve new PIT 
                \* reads at a timestamp before movePrimary commits.
                ELSE [ s \in Shards |-> IF s = toShard THEN LatestUUIDForNameSpace(ns) ELSE LatestShardNamespaceUUID[ns][s] ] 
            ]   ]
        /\ nextUUID' = nextUUID + Cardinality(nsToMove)
    /\ shardData' = [ns \in NameSpaces |-> IF ns \in nsToMove 
        THEN [shardData[ns] EXCEPT ![fromShard] = {}, ![toShard] = shardData[ns][fromShard]]
        ELSE shardData[ns]]
    /\ databaseMetadata' = [primaryShard |-> toShard, dbVersion |-> NextClusterTime]
    /\ clusterTime' = NextClusterTime
    /\ UNCHANGED << collectionMetadata, log, response, router_vars, shardTxnResources >>

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
    \/ \E to \in Shards : MovePrimary(to)
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
    /\ WF_vars(\E to \in Shards : MovePrimary(to))

Spec == /\ Init /\ [][Next]_vars /\ Fairness

----------------------------------------------------------------------------------------------------

(**************************************************************************************************)
(* Type invariants.                                                                               *)
(**************************************************************************************************)

TypeOK ==
    /\  log \in [Txns -> Seq(SUBSET ReqEntry)]
    /\  collectionMetadata \in [ NameSpaces -> CollectionMetadataFormat ]
    /\  rCollectionCache \in [ NameSpaces -> CollectionCacheMetadataFormat ]

ShardDataConsistentWithUUID ==
    /\  \A ns \in NameSpaces:
        /\ (\A s \in Shards : shardData[ns][s] = {}) <=> (\A s \in Shards : LatestShardNamespaceUUID[ns][s] = DroppedNamespaceUUID)
        /\ (\E s \in Shards : shardData[ns][s] # {}) <=> LatestShardNamespaceUUID[ns][databaseMetadata.primaryShard] # DroppedNamespaceUUID

RouterSendsOneStmtRequestPerShard ==
    /\  \A txn \in Txns : 
        /\  \A i \in DOMAIN(log[txn]) :
            LET stmt == log[txn][i]
            IN  \* only one entry per shard
                /\  \A s \in Shards : Cardinality({entry \in stmt : entry.shard = s}) <= 1
                
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

StmtResponsesHaveConsistentKeySet(t, stmt) ==
    LET txnUUIDs == {rsp.snapshot.uuid : rsp \in response[t][stmt]}
        isDropUUID == \E uid \in txnUUIDs: uid = DroppedNamespaceUUID
    IN  \*  Check incarnation uniqueness.
        /\  Cardinality(txnUUIDs) = 1
        \*  Check data set completeness and uniqueness, or completely empty.
        /\  IF ~isDropUUID THEN
                StmtResponseDataCompleteAndUnique(response[t][stmt])
            ELSE
                \A rsp \in response[t][stmt] : rsp.snapshot.data = {}

CommittedTxnImpliesConsistentKeySet == \A t \in Txns: TxnCommitted(t) =>
        \A stmt \in Stmts: 
            /\ StmtResponsesHaveConsistentKeySet(t, stmt)

AllTxnsEventuallyDone == <>[] \A t \in Txns : TxnDone(t)
AcquiredTxnResourcesEventuallyReleased == \A s \in Shards, t \in Txns :
    shardTxnResources[s][t] # EmptyTxnResources ~> shardTxnResources[s][t] = EmptyTxnResources

\* Verifies that a shard responding OK to a request for an untracked collection, only happens when 
\* the shard is the database primary at that moment (1) and the collection only existed in that 
\* shard at the snapshot's timestamp (2). (1) checks the database versioning protocol detects 
\* staleness, and (2) that there are no anomalies due to the established snapshot being incompatible
\* with the latest database version (checked with placementConflictTime).
ResponseForUntrackedNameSpaceIsFromPrimaryShard ==
    [][
        \A txn \in Txns, stmt \in Stmts :
            \A rsp \in response'[txn][stmt] :
            (
                /\ response[txn] # response'[txn]
                /\ response[txn][stmt] # response'[txn][stmt]
                /\ rsp \notin response[txn][stmt]
                /\ rsp.status = OK
                /\ \E req \in log[txn][stmt] : req.collectionGen = UntrackedCollectionGen
            ) => (
                /\ rsp.shard = databaseMetadata.primaryShard
                /\ LET  ts == shardTxnResources'[rsp.shard][txn].snapshot.ts IN
                    ~\E s \in Shards: 
                        /\ s # rsp.shard
                        /\ shardNamespaceUUID[ts][rsp.ns][s] # DroppedNamespaceUUID
            )                        
    ]_response

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