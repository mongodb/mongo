# Distributed CRUDs

On sharded clusters, the router will route CRUD operations to the shard(s) that own the ranges relevant to the query predicate. If the query includes the shard key, then a router will only target the shard(s) that own that shard key (or ranges of shard keys). Conversely, if the query does not include the shard key, then a router will broadcast to all shards that own data for the collection.

Routers use a cache of collection routing tables to determine what shard owns each range of the collection. This cache can sometimes be stale (e.g. after a range migration commits). In order to ensure that the query was routed correctly, the router will use the [placement versioning protocol](https://github.com/mongodb/mongo/blob/master/src/mongo/db/s/README_versioning_protocols.md) when forwarding requests to the shard. This ensures that the routing table used for targeting was not stale — if it was, shards will reject the request and inform the router that its routing information is stale.

If more than one shard was targeted, then the results returned by each shard will be merged by the router which will then return the results to the client.

## Operation

- The router forwards the CRUD operation to the shards owning the relevant ranges.
  - For CRUDs operating with a read concern weaker than snapshot, the latest routing table is consulted. For reads with snapshot read concern, the routing table is interpreted at atClusterTime.
  - If the router receives a staleRouter error from one shard, it closes any established downstream cursor for the CRUD op, refreshes its routing table from the config shard, and retries.
- Each targeted shard:
  - Validates the placement version supplied by the router. Two cases are possible:
    - The supplied placement version matches the shard's latest version, in which case the shard proceeds.
    - The supplied placement version is lower than the shard's, in which case the shard returns a staleRouter error code.
  - If placement version check passes, the shard establishes the cursor and installs the range ownership filter for a tracked namespace according to the query's read timestamp.

## Invariants

- The router supplies a placement version. Failure to do so opens the possibility for a CRUD operation to miss a document, observe/write to a document more than once, or observe an orphaned document.
- The router supplies its latest known placement version for the namespace, regardless of the read concern level or specific read timestamp.
- For each query operation, the router uses the same instance of the routing table–-and placement versions-–to forward requests to the targeted shards. This ensures that targeted shards establish a non-overlapping, complete set of the ranges involved in the query.
- The shard preserves (i.e., does not clean up) the ranges that have been migrated out since the cursor was established. This allows CRUDs operating with a read concern weaker than snapshot, which periodically advance their snapshot, to observe the key set that the shard owned at the time the cursor was established.

## Liveness considerations

- The router should establish cursors on all involved shards early in execution. This avoids a scenario where a potentially long-running query establishes and exhausts a cursor on a shard, then attempts to establish the cursor on a second shard, only to fail because a range migration (placement change) committed in between.
- The range preserver keeps established cursors alive across placement changes, to avoid failing queries due to balancing.

## Isolation considerations

CRUD operations with a read concern weaker than snapshot roughly match the "read committed" ANSI isolation level. Due to the snapshot advancing periodically, an artefact of range preservation is that the CRUD operation can observe an immutable view of a range that's been since migrated out, as opposed to other ranges that continue to observe new writes as they commit. This behavior is acceptable with ANSI read committed.

## See Also

- The [MoveRange TLA+ specification](https://github.com/mongodb/mongo/blob/d40899bd45db62def8941cc6ba65c44a2cbbb83a/src/mongo/tla_plus/MoveRange/MoveRange.tla), which models the distributed query protocol and verifies the safety and liveness properties described in this readme.
- The [Sharded Transactions and DDLs readme](https://github.com/mongodb/mongo/blob/master/src/mongo/db/s/README_transactions_and_ddl.md), covering aspects pertaining to CRUD operations in distributed transactions.
- The [RoutingContext readme](/src/mongo/s/query/README_routing_context.md) for information about routing operations safely with the `RoutingContext`
