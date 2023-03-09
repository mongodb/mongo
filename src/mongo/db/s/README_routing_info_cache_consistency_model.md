> **Warning**
> This is work in progress and some sections are incomplete

# Consistency Model of the Routing Table Cache
This section builds upon the definitions of the sharding catalog in [this section](https://github.com/mongodb/mongo/blob/9b4ddb11af242d7c8d48181c26ca091fe4533642/src/mongo/db/s/README_sharding_catalog.md#catalog-containers) and elaborates on the consistency model of the [CatalogCache](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/s/catalog_cache.h#L134), which is what backs the Router role.

#### Timelines
Let's define the set of operations which a DDL coordinator performs over a set of catalog objects as the **timeline** of that object. The timelines of different objects can be **causally dependent** (or just *dependent* for brevity) on one another or **independent**.

For example, creating a sharded collection only happens after a DBPrimary has been created for the owning database, therefore the timeline of a collection is causally dependent on the timeline of the owning database. Similarly, placing a database on a shard can only happen after that shard has been added, therefore the timeline of a database is dependent on the timeline of the shards data.

On the other hand, two different clients creating two different sharded collections under two different DBPrimaries are two timelines which are independent from each other.

#### Routing info cache objects
The list below enumerates the current set of catalog objects available in the routing info cache, their cardinality (how many exist in the cluster), dependencies and the DDL coordinators which are responsible for their timelines:

* ConfigData: Cardinality = 1, Coordinator = CSRS, Causally dependent on the clusterTime on the CSRS.
* ShardsData: Cardinality = 1, Coordinator = CSRS, Causally dependent on ConfigData.
* Database: Cardinality = NumDatabases, Coordinator = (CSRS with a hand-off to the DBPrimary after creation), Causally dependent on ShardsData.
* Collection: Cardinality = NumCollections, Coordinator = DBPrimary, Causally dependent on Database.
* CollectionPlacement: Cardinality = NumCollections, Coordinator = (DBPrimary with a hand-off to the Donor Shard for migrations), Causally dependent on Collection.
* CollectionIndexes: Cardinality = NumCollections, Coordinator = DBPrimary, Causally dependent on Collection.

#### Consistency model
Since the sharded cluster is a distributed system, it would be prohibitive to have each user operation go to the CSRS in order to obtain an up-to-date view of the routing information. Therefore the cache's consistency model needs to be relaxed.

Currently, the cache exposes a view of the routing table which preserves the causal dependency of only *certain* dependent timelines and provides no guarantees for timelines which are not related.

The only dependent timelines which are preserved are:
 * Everything dependent on ShardsData
 * CollectionPlacement and Collection
 * CollectionIndexes and Collection

What this means is that for each timeline which is causally dependent, if the cache returns a value V, then it will return the causally dependent values for any preserved timelines.

For example, if the CatalogCache returns a chunk which is placed on shard S1, the same caller is guaranteed to see shard S1 in the ShardRegistry, rather than potentially get ShardNotFound. The inverse is not guaranteed: if a shard S1 is found in the ShardRegistry, there is no guarantee that any collections that have chunks on S1 will be in the CatalogCache.

Similarly, because collections have independent timelines, there is no guarantee that if the CatalogCache returns collection C2, that the same caller will see collection C1 which was created earlier in time.

Implementing the consistency model described in the previous section can be achieved in a number of ways which range from always fetching the most up-to-date snapshot of all the objects in the CSRS to a more precise (lazy) fetching of just an object and its dependencies. The current implementation of sharding opts for the latter approach. In order to achieve this, it assigns "timestamps" to all the objects in the catalog and imposes relationships between these timestamps such that the "relates to" relationship is preserved.

The objects and their timestamps are as follows:
 * ConfigData: `configTime`, which is the most recent majority timestamp on the CSRS
 * ShardData: `topologyTime`, which is an always increasing value that increments as shards are added and removed and is stored in the config.shards document
 * Database*: `databaseTimestamp`, which is an always-increasing value that increments each time a database is created or moved
 * CollectionPlacement*: `collectionTimestamp/epoch/majorVersion/minorVersion`, henceforth referred to as the `collectionVersion`
 * CollectionIndexes*: `collectionTimestamp/epoch/indexVersion`, henceforth referred to as the `indexVersion`

Because of the "related to" relationships explained above, there is a strict dependency between the various timestamps:
 * `configTime > topologyTime`: Due to the fact that the topologyTime is chosen before the insert into config.shards happens, it is always less than the oplog entry of the write that added a shard. Because of an implementation detail of the write to config.shards, the chosen `topologyTime` is not gossipped-out until the write is [majority committed](https://github.com/mongodb/mongo/blob/ce925dfbbc9459f65b1ad6f91a2d85c02ab69ca4/src/mongo/db/s/topology_time_ticker.h#L64-L75). When observers see a new `toplogyTime`, they will know that the ShardData has changed and will be able to see the new changes using the latest config time.
 * `databaseTimestamp > topologyTime`: Topology time which includes the DBPrimary Shard
 * `collectionTimestamp > databaseTimestamp`: DatabaseTimestamp which includes the creation of that database

Because every object in the cache depends on the `configTime` and the `topologyTime`, which are singletons in the system, these values are propagated on every communication within the cluster. Any change to the `topologyTime` informs the ShardRegistry that there is new information present on the CSRS, so that a subsequent `getShard` will refresh if necessary (i.e., if the caller asks for a DBPrimary which references a newly added shard).

As a result, the process of sending of a request to a DBPrimary is as follows:
 * Ask for a database object from the CatalogCache
 * The CatalogCache fetches the database object from the CSRS (only if its been told that there is a more recent object in the persistent store), which implicitly fetches the `topologyTime` and the `configTime`
 * Ask for the DBPrimary shard object from the ShardRegistry
 * The ShardRegistry ensures that it has caught up at least up to the topologyTime that the fetch of the DB Primary brought and if necessary reaches to the CSRS

