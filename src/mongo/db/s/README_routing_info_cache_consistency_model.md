# Consistency Model of the Routing Info Cache

This section builds upon the definitions of the sharding catalog in [this section](README_sharding_catalog.md#catalog-containers) and elaborates on the consistency model of the [CatalogCache](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/s/catalog_cache.h#L134), which is what backs the [Router role](README_sharding_catalog.md#router-role).

## Timelines

Let's define the set of operations which a DDL coordinator performs over a set of catalog objects as the **timeline** of that object. The timelines of different objects can be **causally dependent** (or just _dependent_ for brevity) on one another, or they can be **independent**.

For example, creating a sharded collection only happens after a DBPrimary has been created for the owning database, therefore the timeline of a collection is causally dependent on the timeline of the owning database. Similarly, placing a database on a shard can only happen after that shard has been added, therefore the timeline of a database is dependent on the timeline of the shards data.

On the other hand, two different clients creating two different sharded collections under two different DBPrimaries are two timelines which are independent from each other.

## Routing info cache objects

The list below enumerates the current set of catalog objects in the routing info cache, their cardinality (how many exist in the cluster), their dependencies and the DDL coordinators which are responsible for their timelines:

-   ConfigData: Cardinality = 1, Coordinator = CSRS, Causally dependent on the clusterTime on the CSRS.
-   ShardsData: Cardinality = 1, Coordinator = CSRS, Causally dependent on ConfigData.
-   Database: Cardinality = NumDatabases, Coordinator = (CSRS with a hand-off to the DBPrimary after creation), Causally dependent on ShardsData.
-   Collection: Cardinality = NumCollections, Coordinator = DBPrimary, Causally dependent on Database.
-   CollectionPlacement: Cardinality = NumCollections, Coordinator = (DBPrimary with a hand-off to the Donor Shard for migrations), Causally dependent on Collection.
-   CollectionIndexes: Cardinality = NumCollections, Coordinator = DBPrimary, Causally dependent on Collection.

## Consistency model

Since the sharded cluster is a distributed system, it would be prohibitive to have each user operation go to the CSRS in order to obtain an up-to-date view of the routing information. Therefore the cache's consistency model needs to be relaxed.

Currently, the cache exposes a view of the routing table which preserves the causal dependency of only _certain_ dependent timelines and provides no guarantees for timelines which are not related.

The only dependent timelines which are preserved are:

-   Everything dependent on ShardsData: Meaning that if a database or collection placement references shard S, then shard S will be present in the ShardRegistry
-   CollectionPlacement and Collection: Meaning that if the cache references placement version V, then it will also reference the collection description which corresponds to that placement
-   CollectionIndexes and Collection: Meaning that if the cache references index version V, then it will also reference the collection description which corresponds to that placement

For example, if the CatalogCache returns a chunk which is placed on shard S1, the same caller is guaranteed to see shard S1 in the ShardRegistry, rather than potentially get ShardNotFound. The inverse is not guaranteed: if a shard S1 is found in the ShardRegistry, there is no guarantee that any collections that have chunks on S1 will be in the CatalogCache.

Similarly, because collections have independent timelines, there is no guarantee that if the CatalogCache returns collection C2, that the same caller will see collection C1 which was created earlier in time.

Implementing the consistency model described in the previous section can be achieved in a number of ways which range from always fetching the most up-to-date snapshot of all the objects in the CSRS to a more precise (lazy) fetching of just an object and its dependencies. The current implementation of sharding opts for the latter approach. In order to achieve this, it assigns "timestamps" to all the objects in the catalog and imposes relationships between these timestamps such that the "relates to" relationship is preserved.

### Object timestamps

The objects and their timestamps are as follows:

-   ConfigData: `configTime`, which is the most recent majority timestamp on the CSRS
-   ShardData: `topologyTime`, which is an always increasing value that increments as shards are added and removed and is stored in the config.shards document
-   Database\*: `databaseTimestamp`, which is an always-increasing value that increments each time a database is created or moved
-   CollectionPlacement\*: `collectionTimestamp/epoch/majorVersion/minorVersion`, henceforth referred to as the `collectionVersion`
-   CollectionIndexes\*: `collectionTimestamp/epoch/indexVersion`, henceforth referred to as the `indexVersion`

Because of the "related to" relationships explained above, there is a strict dependency between the various timestamps (please refer to the following section as well for more detail):

-   `configTime > topologyTime`: If a node is aware of `topologyTime`, it will be aware of the `configTime` of the write which added the new shard (please refer to the section on [object timestamps selection](#object-timestamps-selection) for more information of why the relationship is "greater-than")
-   `databaseTimestamp > topologyTime`: Topology time which includes the DBPrimary Shard (please refer to the section on [object timestamps selection](#object-timestamps-selection) for more information of why the relationship is "greater-than")
-   `collectionTimestamp > databaseTimestamp`: DatabaseTimestamp which includes the creation of that database

Because every object in the cache depends on the `configTime` and the `topologyTime`, which are singletons in the system, these values are propagated on every communication within the cluster. Any change to the `topologyTime` informs the ShardRegistry that there is new information present on the CSRS, so that a subsequent `getShard` will refresh if necessary (i.e., if the caller asks for a DBPrimary which references a newly added shard).

As a result, the process of sending of a request to a DBPrimary is as follows:

-   Ask for a database object from the CatalogCache
-   The CatalogCache fetches the database object from the CSRS (only if its been told that there is a more recent object in the persistent store), which implicitly fetches the `topologyTime` and the `configTime`
-   Ask for the DBPrimary shard object from the ShardRegistry
-   The ShardRegistry ensures that it has caught up at least up to the topologyTime that the fetch of the DB Primary brought and if necessary reaches to the CSRS

## Object timestamps selection

In the replication subsystem, the optime for an oplog entry is usually generated when that oplog entry is written to the oplog. Because of this, it is difficult to make an oplog entry to contain its own optime, or for a document to contain the optime of when it was written.

As a consequence of the above, since the `topologyTime`, `databaseTimestamp` and `collectionTimestamp` are chosen before the write to the relevant collection happens, it is always less than the oplog entry of that write. This is not a problem, because none of these documents are visible before the majority timestamp has advanced to include the respective writes.

For the `topologyTime` in particular, it is not gossiped-out until the write is [majority committed](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/s/topology_time_ticker.h#L64-L80).
