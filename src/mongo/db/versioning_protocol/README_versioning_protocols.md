# Versioning Protocols

When a command is sent to a router, the router must decide which shards to forward this request to - a process called routing. Routing in MongoDB acts optimistically, meaning that a router will use on whatever information it has cached to decide which shards to send the request to, and then rely on the shards to return an error if this information is stale.

This process is implemented via the shard versioning protocol and it is what prevents the config servers from becoming a bottleneck for commands while ensuring that the router eventually sends the command to the correct set of shards.

## Shard Versioning Protocol

Some operations are always to be executed by the _db-primary_ shard for a given database. Each database in a sharded cluster has a primary shard. This shard is responsible for coordinating DDL operations and reads/writes to _untracked_ unsharded collections (which have no metadata stored on the config server). For these types of operations the router uses its database placement cache to decide what shard to send the request to, and will attach a [`databaseVersion`](#database-version) token to the request that signifies the version of its cached database placement information.

Other operations are to be targeted to whichever shard(s) own data (or relevant chunk ranges) for a particular tracked/sharded collection. For these the router will use its cached _routing table_ for that collection to decide what shards to target. Later, when sending the requests to them, it will attach the [`shardVersion`](#shard-version) token that represents the routing table version it considered.

When a shard receives the request, it will check this token to make sure that it matches the shard's local information. If it matches, then the request will proceed. If the version does not match, the shard will throw [an exception](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/s/stale_exception.h).

When the router receives this exception, it knows that the routing information must have changed, and so it will [perform a refresh](#routing-information-refreshes) to get more recent information before sending the request again.

The following diagram depicts a simple example of the shard versioning protocol in action. It assumes that the router is a shard server primary, thus the refresh is simply fetching newer information from the config server.

```mermaid
sequenceDiagram
    participant S as Router
    participant C as CSRS
    participant S1 as Shard 1
    participant S2 as Shard 2

    note over C,S2: Initial routing info:<br/>All data on Shard 1<br/>S1 = S1V1

    S->>C: getCollectionRoutingInfo
    C->>S: Shard 1, S1V1
    Note right of S: The routing info tells the router<br/>the set of shards that should be<br/>contacted for the request plus<br/>the shard version for this routing<br/>info.

    Note over C,S2: MoveRange - data is now distributed<br/>between Shards 1 and 2<br/>S1 = S1V2, S2 = S2V2

    S->>S1: Req, S1V1
    Note left of S1: Since Shard 1 has ShardVersion<br/>S1V2, it rejects the request with<br/>ShardVersion S1V1, sending<br/>StaleConfig back to router
    S1->>S: StaleConfig(S1V2)
    Note right of S: The StaleConfig error tells the<br/>router to fetch updated<br/>information from the config<br/>server before retrying the<br/>request.
    S->>C: getCollectionRoutingInfo
    C->>S: Shard 1, S1V2<br/>Shard 2, S2V2
    Note right of S: The new routing info tells the<br/>router that the request should<br/>be sent to both Shard 1 and<br/>Shard 2.
    S->>S1: Req, S1V2
    S->>S2: Req, S2V2
    Note over S1,S2: Since the shard versions sent<br/>match what the shards have<br/>locally, they accept the<br/>request from by the router
    S1->>S: OK
    S2->>S: OK
```

The protocol is the same when using a `databaseVersion`, the only difference is that StaleDbRoutingVersion is returned to the router instead of StaleConfig. In practice, both the Database Version and Shard Version are more complicated than an increasing integer, and their components are described below.

## Database Version

A database version is represented as DBV<U, T, Mod> and consists of three elements:

1. **U** (the uuid) : a unique identifier to distinguish different instances of the database. The UUID remains unchanged for the lifetime of the database, changing when the database is dropped and recreated.
2. **T** (the timestamp) : a unique identifier introduced in version 5.0. The difference between the uuid and timestamp is that timestamps are comparable, allowing for ordering database versions in which the UUID/Timestamp do not match. Like the UUID, the timestamp changes when the database is dropped and recreated, but unlike the UUID, the timestamp also changes the database changes its primary shard.
3. **M** (last modified) : an integer incremented when the database changes its primary shard.

## Shard Version

The shard version is represented as SV<E, T, M, m> and consists of four elements:

1. **E** (the epoch) : a unique identifier that distinguishes an instance of the collection.
2. **T** (the timestamp) : a new unique identifier introduced in version 5.0. The difference between the epoch and timestamp is that timestamps are comparable, allowing for ordering shard versions in which the epoch/timestamp do not match.
3. **M** (major version) : an integer used to indicate a change in data placement, as from a migration.
4. **m** (minor version) : an integer used to indicate a change to data boundaries within a shard such as from a split or merge.

The epoch and timestamp serve the same functionality, that of uniquely identifying an instance of the collection. For this reason, we group them together and call them the [**collection generation**](https://github.com/mongodb/mongo/blob/10fd84b6850ef672ff6ed367ca9292ad8db262d2/src/mongo/s/chunk_version.h#L38-L80). Likewise, the major and minor versions work together to describe the layout of data on the shards. Together, they are called the [**collection placement**](https://github.com/mongodb/mongo/blob/10fd84b6850ef672ff6ed367ca9292ad8db262d2/src/mongo/s/chunk_version.h#L82-L113) (or placement version).

```mermaid
classDiagram
    ShardVersion--CollectionGeneration
    CollectionGeneration--CollectionPlacement

    link ShardVersion "https://github.com/mongodb/mongo/blob/r6.2.1/src/mongo/s/shard_version.h"
    link CollectionGeneration "https://github.com/mongodb/mongo/blob/10fd84b6850ef672ff6ed367ca9292ad8db262d2/src/mongo/s/chunk_version.h#L38-L80"
    link CollectionPlacement "https://github.com/mongodb/mongo/blob/10fd84b6850ef672ff6ed367ca9292ad8db262d2/src/mongo/s/chunk_version.h#L82-L113"
end
```

A change in the CollectionGeneration implies that the CollectionPlacement must have changed as well, since the collection itself has changed.

Each shard has its own shard version, which consists of the collection generation and the maximum placement version of the ranges located on the shard. Similarly, the overall collection version consists of the collection generation and the maximum placement version of any range in the collection.

> [!NOTE]
> Both mongod and mongos processes log Shard Version values [using](https://github.com/mongodb/mongo/blob/6dfd42e971cb39367418203ed8137d423e28df3d/src/mongo/db/versioning_protocol/chunk_version.cpp#L75-L77) the `M|m||E||T` format.

### Operations that change the shard versions

Changes of the shard version indicate that some routing information has changed, and routers need to request updated information. Changes in different components of the shard version indicate different routing information changes.

#### Generation Changes

A change in the collection generation indicates that the collection has changed so significantly that all previous placement information is incorrect. Changes in this component can be caused by dropping and recreating the collection, refining its shard key, renaming it, or resharding it. This will indicate that all routing information is stale, and all routers need to fetch new information.

#### Placement Version Changes

A placement version change indicates that something has changed about what data is placed on what shard. The most important operation that changes the placement version is migration, however split, merge and even some other operations change it as well, even though they don't actually move any data around. These changes are more targeted than generation changes, and will only cause the router to refresh if it is targeting a shard that was affected by the operation.

## Routing (a.k.a. Placement) Information Refreshes

For sharded collections, routing is driven by the chunk placement information, for which the config server represents the [authoritative source](../local_catalog/README_sharding_catalog.md#authoritative-containers).

MongoS and shard primaries refresh their placement information from the config server. Shard secondaries, however, refresh from the shard primaries through a component called the Shard Server Catalog Cache Loader. When a shard primary refreshes from a config server, it persists the refreshed information to disk. This information is then replicated to secondaries who will refresh their cache from this on-disk information.

#### Incremental and Full Refreshes

A full refresh clears all cached information, and replaces the cache with the information that exists on the node’s source whereas an incremental refresh only replaces modified routing information from the node’s source.

Incremental refreshes will happen whenever there has been a [placement version change](#placement-version-changes), while [collection generation changes](#generation-changes) will cause a full refresh.
