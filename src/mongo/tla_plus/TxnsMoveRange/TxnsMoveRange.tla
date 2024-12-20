\* Copyright 2024 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------------------- MODULE TxnsMoveRange -----------------------------------------
\* This is the formal specification for the multi-statement transactions data consistency
\* protocol, with sub-snapshot read concern level and in the presence of range migrations.
\* It covers the placement versioning protocol, and the `placementConflictTime' protocol that
\* forbids the data placement anomaly described at:
\* https://github.com/mongodb/mongo/blob/master/src/mongo/db/s/README_transactions_and_ddl.md
\*
\* To run the model-checker, first edit the constants in MCTxnsMoveRange.cfg if desired, then:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh TxnsMoveRange

EXTENDS Integers, Sequences, FiniteSets, TLC
CONSTANTS
    Shards,
    Keys,
    NameSpaces,
    Txns,
    MIGRATIONS,
    TXN_STMTS

    staleRouter  == "staleRouter"
    snapshotIncompatible  == "snapIncompatible"
    ok   == "ok"

ASSUME Cardinality(Shards) > 1
ASSUME Cardinality(Keys) > 0
ASSUME Cardinality(NameSpaces) > 0
ASSUME Cardinality(Txns) > 0
ASSUME MIGRATIONS \in 0..100
ASSUME TXN_STMTS \in 1..100

INIT_MIGRATION_TS == 100 \* Arbitrary initial timestamp.
MIGRATION_TS_DOMAIN == INIT_MIGRATION_TS..INIT_MIGRATION_TS+MIGRATIONS
Stmts == 1..TXN_STMTS
Status == {staleRouter, snapshotIncompatible, ok}

\* A router request.
\* 's' is the target shard. 'ns' is the target namespace. 'k' is the query predicate (a key match).
\* 'v' is the placement version. 'placementConflictTs' is the placementConflictTimestamp.
ReqEntry == [s: Shards, ns: NameSpaces, k: Keys, v: 0..MIGRATIONS,
             placementConflictTs: MIGRATION_TS_DOMAIN]
CreateReq(s, ns, k, v, pcts) == [s |->s, ns |-> ns, k |-> k, v |-> v, placementConflictTs |-> pcts]

\* A shard's response to a router request. 'found' indicates whether the key matched in the shard.
RspEntry == [rsp: {staleRouter, snapshotIncompatible, ok}, found: {TRUE, FALSE}]
CreateRsp(rsp, found) == [rsp |-> rsp, found |-> found]
HasResponse(stmt) == Cardinality(DOMAIN stmt) # 0

RespondStatus(receivedPlacementV, shardLastPlacementV, placementConflictTs, shardLastMigrationTs) ==
    IF receivedPlacementV < shardLastPlacementV THEN staleRouter
        ELSE IF placementConflictTs < shardLastMigrationTs THEN snapshotIncompatible
            ELSE ok

Max(S) == CHOOSE x \in S : \A y \in S : x >= y

(* Global and networking variables *)
VARIABLE ranges             \* The authoritative routing table the router populates its cache from.
VARIABLE versions           \* The versioning of the authoritative routing table.
VARIABLES request, response \* Emulate RPC with queues between the router and the shards.
VARIABLE migrations         \* Ancillary variable limiting the number of migrations.

(* Router variables *)
VARIABLE rCachedRanges          \* The cached routing table. Lazily populated from `ranges'.
VARIABLE rCachedVersions        \* The version associated with the cached routing table.
VARIABLE rCompletedStmt         \* Router statements acknowledged by the shard.
VARIABLE rPlacementConflictTs   \* Per-transaction, immutable timestamp that the router forwards
                                \* with each statement. Used to detect the data placement anomaly.

(* Shard variables *)
VARIABLE shardLastMigrationTs   \* The timestamp of the last committed incoming range migration.
VARIABLE shardSnapshotted       \* Whether the shard has established a snapshot for a transaction.
VARIABLE shardSnaspshot         \* The transaction's storage snapshot on the shard.
VARIABLE shardLocked            \* Per-namespace intent lock. Serializes with range migrations.

vars == <<ranges, versions, request, response, shardLocked, rCachedVersions, rCachedRanges,
          rPlacementConflictTs, rCompletedStmt, shardLastMigrationTs, shardSnapshotted,
          shardSnaspshot, migrations>>
\* Variables grouped by writer.
router_vars == <<rCachedRanges, rCachedVersions, rPlacementConflictTs, rCompletedStmt, request>>
shard_vars == <<shardSnapshotted, shardSnaspshot, response>>
migration_vars== <<shardLastMigrationTs, ranges, versions, migrations>>

Init == (* Global and networking *)
        /\ ranges \in [NameSpaces -> [Keys -> Shards]]
        /\ versions = [s \in Shards |-> [n \in NameSpaces |-> 0]]
        /\ request = [t \in Txns |-> <<>>]
        /\ response = [t \in Txns |-> [stm \in Stmts |-> <<>>]]
        /\ migrations = 0
        (* Router *)
        /\ rCachedVersions = [n \in NameSpaces |-> [s \in Shards |-> 0]]
        /\ rCachedRanges = [n \in NameSpaces |-> ranges[n]]
        /\ rPlacementConflictTs = [t \in Txns |-> -1]
        /\ rCompletedStmt = [t \in Txns |-> 0]
        (* Shard *)
        /\ shardSnapshotted = [self \in Shards |-> [t \in Txns |-> FALSE]]
        /\ shardSnaspshot = [self \in Shards |-> [t \in Txns |-> [n \in NameSpaces |-> {}]]]
        /\ shardLastMigrationTs = [s \in Shards |-> [n \in NameSpaces |-> INIT_MIGRATION_TS]]
        /\ shardLocked = [s \in Shards |-> [t \in Txns |-> [n \in NameSpaces |-> FALSE]]]

LatestMigrationTs == Max(UNION {{shardLastMigrationTs[n][x] :
                                    x \in DOMAIN shardLastMigrationTs[n]} :
                                        n \in DOMAIN shardLastMigrationTs})
TxnCommitted(t) == HasResponse(response[t][TXN_STMTS]) /\ response[t][TXN_STMTS]["rsp"] = ok
TxnAborted(t) == \E s \in Stmts : HasResponse(response[t][s]) /\ response[t][s]["rsp"] \notin {ok}
IsNamespaceLocked(s, ns) == \E t \in Txns : shardLocked[s][t][ns]
Lock(lk, t, s, ns) == [lk EXCEPT ![s][t][ns] = TRUE]
Unlock(t) == [s \in Shards |->
                [txn \in Txns |->
                    [ns \in NameSpaces |-> IF txn = t THEN FALSE ELSE shardLocked[s][txn][ns]]]]

\* Action: the router forwards a transaction statement to the shard owning the key.
RouterSendTxnStmt(t, ns, k) ==
    /\ Len(request[t]) < TXN_STMTS
    /\ rCompletedStmt[t]=Len(request[t])
    /\ IF rPlacementConflictTs[t] = -1
        \* Choose the transaction's placementConflictTimestamp. By design, this is chosen as the
        \* latest clusterTime known by the router. This model simplifies the design as follows:
        \*  (a) the clusterTime only ticks when a range migrations commits.
        \*  (b) the router is omniscient of the global latest clusterTime.
        \* The implication of (b) is that in this model, a snapshotIncompatible conflict cannot
        \* occur on the first statement.
        THEN rPlacementConflictTs' = [rPlacementConflictTs  EXCEPT ![t] = LatestMigrationTs]
        ELSE UNCHANGED rPlacementConflictTs
    /\ LET s == rCachedRanges[ns][k] IN
        /\ request' = [request EXCEPT ![t] = Append(request[t],
                                                    CreateReq(s, ns, k,
                                                              rCachedVersions[ns][s],
                                                              rPlacementConflictTs'[t]))]
    /\ UNCHANGED <<shard_vars, migration_vars, rCompletedStmt, rCachedRanges, rCachedVersions,
                   shardLocked>>

\* Action: router processes a non-ok response from a shard.
RouterHandleAbort(t, stm) ==
    /\ HasResponse(response[t][stm])
    /\ rCompletedStmt[t]<stm
    /\ response[t][stm]["rsp"] \notin {ok}
    /\ IF response[t][stm]["rsp"] = staleRouter
        THEN
            \* Refresh the cached routing table for the namespace. Note that refreshes occur lazily
            \* upon receipt of a staleRouter response.
            /\ LET ns == request[t][Len(request[t])]["ns"] IN
                /\ rCachedVersions' =
                    [rCachedVersions EXCEPT ![ns] = [x \in Shards |-> versions[x][ns]]]
                /\ rCachedRanges' = [rCachedRanges EXCEPT ![ns] = ranges[ns]]
        ELSE UNCHANGED <<rCachedVersions, rCachedRanges>>
    /\ rCompletedStmt' = [rCompletedStmt EXCEPT ![t] = TXN_STMTS]
    /\ shardLocked' = Unlock(t) \* Avoid a ShardAbortTxn action to spare state space.
    /\ UNCHANGED <<shard_vars, migration_vars, request, rPlacementConflictTs>>

\* Action: router processes an ok response from a shard.
RouterHandleOk(t, stm) ==
    /\ HasResponse(response[t][stm])
    /\ rCompletedStmt[t]<stm
    /\ response[t][stm]["rsp"] \notin  {staleRouter, snapshotIncompatible}
    /\ rCompletedStmt' = [rCompletedStmt EXCEPT ![t] = rCompletedStmt[t]+1]
    \* Avoid a ShardCommitTxn action to spare state space.
    /\ IF rCompletedStmt'[t] = TXN_STMTS THEN shardLocked' = Unlock(t) ELSE UNCHANGED shardLocked
    /\ UNCHANGED <<shard_vars, migration_vars, rCachedVersions, rCachedRanges, rPlacementConflictTs,
                   request>>

\* Action: shard responds to a statement forwarded by the router.
ShardRespond(t, self) ==
    /\ LET  ln == Len(request[t])
            req == request[t][ln] IN
        /\ ln > 0
        /\ req.s = self
        /\ response[t][ln] = <<>>
        /\ IF shardSnapshotted[self][t] = FALSE THEN
                /\ shardSnapshotted' = [shardSnapshotted EXCEPT ![self][t] = TRUE]
                /\ shardSnaspshot' = [shardSnaspshot EXCEPT ![self][t] =
                    [n \in NameSpaces |-> {k \in DOMAIN(ranges[n]) : ranges[n][k] = self}]]
            ELSE
                /\ UNCHANGED <<shardSnaspshot, shardSnapshotted>>
        /\ response' = [response EXCEPT ![t][ln] =
                            CreateRsp(RespondStatus(req.v, versions[self][req.ns],
                                                    req.placementConflictTs,
                                                    shardLastMigrationTs[self][req.ns]),
                                                    req.k \in shardSnaspshot'[self][t][req.ns])]
        /\ shardLocked' = Lock(shardLocked, t, req.s, req.ns)
    /\ UNCHANGED <<router_vars, migration_vars>>

\* Action: shard migrates one of its keys to another shard.
MoveRange(ns, k, from, to) ==
    /\ migrations < MIGRATIONS
    /\ from = ranges[ns][k]
    /\ to # from
    /\ LET v == Max({versions[s][ns]: s \in Shards}) IN
        \* A donor/recipient shard pair is eligible for migration if they have not established a
        \* lock on the namespace. Interestingly, this spec still satisfies the correctness
        \* properties even if we ignored these locking preconditions entirely. We decided to add
        \* these locking preconditions to avoid exploring transitions that are not possible by
        \* design, and to significantly reduce the state space.
        /\ ~IsNamespaceLocked(from, ns)
        /\ ~IsNamespaceLocked(to, ns)
        /\ versions' = [versions EXCEPT ![from][ns] = v + 1,
                                        ![to][ns] = v + 1]
        /\ ranges' = [ranges EXCEPT ![ns][k] = to]
        \* Only update the "last migration" timestamp on the recipient shard.
        /\ shardLastMigrationTs' = [shardLastMigrationTs EXCEPT ![to][ns] = LatestMigrationTs + 1]
    /\ migrations' = migrations+1
    /\ UNCHANGED <<router_vars, shard_vars, shardLocked>>

Next ==
    \* Router actions
    \/ \E t \in Txns, ns \in NameSpaces, k \in Keys: RouterSendTxnStmt(t, ns, k)
    \/ \E t \in Txns, stm \in Stmts: RouterHandleAbort(t, stm)
    \/ \E t \in Txns, stm \in Stmts: RouterHandleOk(t, stm)
    \* Shard actions
    \/ \E s \in Shards, t \in Txns: ShardRespond(t, s)
    \/ \E ns \in NameSpaces, k \in Keys, from, to \in Shards: MoveRange(ns, k, from, to)
    \* Termination
    \/ UNCHANGED vars

Fairness == TRUE
    /\ WF_vars(\E t \in Txns, ns \in NameSpaces, k \in Keys : RouterSendTxnStmt(t, ns, k))
    /\ WF_vars(\E t \in Txns, stm \in Stmts: RouterHandleAbort(t, stm))
    /\ WF_vars(\E t \in Txns, stm \in Stmts: RouterHandleOk(t, stm))
    /\ WF_vars(\E ns \in NameSpaces, k \in Keys, from, to \in Shards: MoveRange(ns, k, from, to))
    /\ WF_vars(\E s \in Shards, t \in Txns: ShardRespond(t, s))

Spec == /\ Init /\ [][Next]_vars /\ Fairness

----------------------------------------------------------------------------------------------------

(**************************************************************************************************)
(* Type invariants.                                                                               *)
(**************************************************************************************************)

TypeOK ==
    /\ request \in [Txns -> Seq(ReqEntry)]
    /\ ranges \in [NameSpaces -> [Keys -> Shards]]
    /\ versions \in [Shards -> [NameSpaces -> 0..MIGRATIONS]]
    /\ rCachedVersions \in [NameSpaces -> [Shards -> -1..MIGRATIONS]]
    /\ shardLastMigrationTs \in [Shards -> [NameSpaces -> MIGRATION_TS_DOMAIN]]

(**************************************************************************************************)
(* Miscellaneous properties for exploring/understanding the spec.                                 *)
(**************************************************************************************************)

\* Namespaces involved in a transaction
NSsetofTxn(t) == {request[t][x].ns : x \in 1..Len(request[t])}

\* Shards involved in a transaction
ShardsetofTxn(t) == {request[t][x].s : x \in 1..Len(request[t])}

\* shard-ns tuples in a transaction
SNStuplesetofTxn(t) == {<<request[t][x].s, request[t][x].ns>> : x \in 1..Len(request[t])}

(**************************************************************************************************)
(* Correctness Properties                                                                         *)
(**************************************************************************************************)

CommittedTxnImpliesAllStmtsSuccessful ==
    \A t \in Txns: TxnCommitted(t) => \A s \in Stmts : response[t][s]["rsp"] = ok
CommittedTxnImpliesKeysAreVisible ==
    \A t \in Txns: TxnCommitted(t) => \A s \in Stmts : response[t][s]["found"] = TRUE

AllTxnsEventuallyDone == <> \A t \in Txns : TxnCommitted(t) \/ TxnAborted(t)
AllLocksEventuallyRescinded ==
    <>(shardLocked = [s \in Shards |-> [t \in Txns |-> [n \in NameSpaces |-> FALSE]]])

====================================================================================================
