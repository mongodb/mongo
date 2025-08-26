# Sharded Time-Series Collections

For a general overview about how time-series collection are implemented, see [db/timeseries/README.md](../timeseries/README.md).
For an overview of query optimizations for time-series collections see [query/timeseries/README](../query/timeseries/README.md).
This section will focus on the implementation of sharded time-series collection, and assumes knowledge of time-series collections and sharding basics.

## Creating a sharded time-series collection

### shardCollection command

Users can create a sharded time-series collection by running `shardCollection` on the **view** namespace
with the `timeseries` option. This will implicitly create a time-series collection if it doesn't exist.

The shard key pattern must meet all the existing restrictions and the following unique restrictions:

1. Must be either the `timeField`, `metaField`, and/or a subfield of the `metaField`.
2. The `timeField` must appear last if the shard key is compound.
3. The `timeField` can only have an ascending range key.

The primary shard (within the create collection DDL coordinator) will transform the `shardCollection`
command to a command on the **buckets** namespace, and convert the shard key to be on the buckets collection.
The command will then run as a typical `shardCollection` command. This means that the information
persisted on the sharding catalog (collection name and shard key in `config.collections`,
chunk boundaries in `config.chunks`) will reference the buckets namespace and its metadata.

The table below shows how the shard key and the index backing the shard key is converted for sharded
time-series collections. For this example, the `timeseries` options are `{timeField: "t", metaField: "m"}`.

| shard key on the view (as specified by the user in create/shardCollection) | shard key on the buckets (as persisted in config.collections) | Index on the buckets                                    |
| -------------------------------------------------------------------------- | ------------------------------------------------------------- | ------------------------------------------------------- |
| `{t: 1}`                                                                   | `{control.min.t: 1}`                                          | `{control.min.t: 1, control.max.t: 1}`                  |
| `{m: 1, t: 1}`                                                             | `{meta: 1, control.min.t: 1}`                                 | `{meta: 1, control.min.t: 1, control.max.t: 1}`         |
| `{m: "hashed" }`                                                           | `{meta: "hashed" }`                                           | `{meta: "hashed" }`                                     |
| `{m: "hashed", t: 1 }`                                                     | `{meta: "hashed", control.min.t: 1 }`                         | `{meta: "hashed", control.min.t: 1, control.max.t: 1 }` |
| `{m.a: 1 }`                                                                | `{meta.a: 1 }`                                                | `{meta.a: 1 }`                                          |

### Sharding metadata

For viewful timeseries collections, only the buckets collection (not the view) is stored on the
config server. The config server has a
`timeseriesFields` parameter set for each buckets collection, which is identical to `timeseriesOptions`.
The `timeseriesFields` parameter is then loaded in memory by the `CatalogCache`, `ChunkManager`, and
the collection metadata.

For viewless timeseries collections, there is no view namespace, so the config server has all of the
metadata already in the `timeseriesFields` parameter.

If the granularity is updated through a `collMod` command, the config server `timeseriesFields` parameter
will also be updated. We treat the granularity value on the config server as the source of truth for the
granularity value of the collection.

During the `collMod` command, all queries must run with the updated granularity value. It's important
because the `control.min.<timeField>` is set by rounding down the document's `timeField` value
by the specified granularity. If the shard key is on the `timeField`, documents are routed to shards based
on `control.min.<timeField>` which relies on the granularity value.

That is why before performing any CRUD operations or aggregations on the shards, the granularity
on the config server is checked through the cached `CollectionRoutingInfo`. Therefore, operations will
always run with the most up to date granularity value, and thus predicates will be routed correctly.

### How chunks are formatted

The shard key can be on the `metaField` or any number of subfields of the `metaField`. This is the
recommended approach, since users should choose a `metaField` that will partition the measurements in
a slightly uniform way. In contrast, `timeField` values are monotonically increasing and thus could route all inserts to a single shard.

If the shard key is on the `timeField` the chunk ranges will be defined on the buckets collection on
the `control.min.<timeField>` field. The `control.min.<timeField>` value is a rounded down lower boundary
for the bucket. It's possible (and likely) that no measurements (user documents) have this value.

Unlike normal sharded collections, a measurement’s location is not tightly bound to the chunk range, because the chunk range defines
where the **buckets** not measurements should go. Usually, a chunk range would never overlap with another chunk; we can assume all
measurements with a certain value defined by the chunk exist only on one chunk. However, this is not the case for time-series.
Bucket ranges do overlap and can belong to different chunks. This means that measurements that have values
exceeding the maximum value of the chunk range can exist on the chunk.

We will illustrate this with an example where the shard key is on the `timeField` and the `timeField = time`:

```
// We have the following measurements:
Doc1 {time: TimeStamp(10000), A:10, B:11, C:12}
Doc2 {time: TimeStamp(25000), A:20, B:21, C:22}
Doc3 {time: TimeStamp(30000), A:30, B:31, C:32}

// We have 3 buckets:
Bucket1 {control.min.time: TimeStamp(10000), control.max.time: TimeStamp(25000), <bucketed measurements>}
Bucket2 {control.min.time: TimeStamp(15000), control.max.time: TimeStamp(30000), <bucketed measurements>}
Bucket3 {control.min.time: TimeStamp(20000), control.max.time: TimeStamp(35000), <bucketed measurements>}

// We have a this possible set of chunks:
Chunk0 range: {control.min.time: MinKey, control.min.time: TimeStamp(10000)}
Chunk1 range: {control.min.time: TimeStamp(10000), control.min.time: TimeStamp(20000))}
Chunk2 range: {control.min.time: TimeStamp(20000), control.min.time: TimeStamp(30000)}
Chunk3 range: {control.min.time: TimeStamp(30000), control.min.time: MaxKey}

// Where the buckets and measurements are:
Chunk0 contains no buckets
Chunk1 contains Bucket1 and Bucket2. Bucket1 contains Doc1. Bucket2 contains Doc2.
Chunk2 contains Bucket3. Bucket3 contains Doc3.
Chunk3 contains no buckets

```

`Chunk1` contains `Bucket2`, but `Bucket2` has a measurement (`Doc2`) which is outside the chunk boundary,
but the measurement fits inside the bucket. Therefore, chunk boundaries in time-series collections do not
define where measurements are stored.

## CRUD operations

For inserts/updates/delete requests, mongos will receive the request on the **view** namespace and check
if the chunk manager has a routing table for the buckets collection. If it doesn't find one, it will check if a
buckets collection exists in the `CatalogCache` (see `CollectionRoutingInfoTargeter::_init`). If it finds a
buckets collection in either location, mongos will do the following:

1. Translate the request to be on the buckets collection namespace.
2. Extract the buckets collection's shard key.
3. Set the `isTimeSeriesNameSpace` flag to `true`.
4. For updates and deletes: Rewrite the query predicate (see `getBucketLevelPredicateForRouting`). The
   field names are changed to match the buckets (`metaField` will become `"meta"`, and `timeField` will
   become `control.min.<timeField>` and `control.max.<timeField>`). This rewritten predicate is used
   for routing.

These steps allow mongos to decide which shards to target, or if to broadcast the command. For example,
if the shard key is on the `metaField`, and there is no predicate on the `metaField` step 4 won't rewrite the
predicate and will pass an empty object into the shard key extractor. An empty object will also be passed
into the shard key extractor if there is a shard key on the `timeField` and no predicate on the `timeField`.
This will trigger mongos to broadcast the update/delete request.

After mongos routes or broadcasts the request, the shards receive it. Then the shards check if the `isTimeSeriesNameSpace`
is set (see `timeseries::isTimeseriesViewRequest`). If it is set, the shards call specific time-series functions, just
like unsharded time-series collections. For example, for inserts, measurements will try to be inserted into an open bucket
in the bucket catalog, then a reopened bucket, and finally a new bucket will be opened if necessary. Updates
and deletes occur one bucket at a time, and the buckets will be unpacked if necessary. See
[db/timeseries/README.md](../../../db/timeseries/README.md) for more details about the specific implementations
of each CRUD operation.

## Query routing for aggregation

### Viewful timeseries collections

This works similarly to queries on a view of a normal sharded collection. Users write a query on the
**view** namespace. Mongos routes the query to the primary shard. The primary shard resolves the view,
rewrites the query to be on the buckets collection, and throws a `CommandOnShardedViewNotSupportedOnMongod`
with the entire pipeline view definition in the returned error message. Mongos receives the expanded
view definition, and then routes the query as it typically would.

The aggregation stage to handle time-series buckets collections (`$_internalUnpackBucket`) is pushed
down to the shards. For more information about `$_internalUnpackBucket` and query rewrites see
[query/timeseries/README](../query/timeseries/README.md).

### Viewless timeseries collections

[TODO (SERVER-102458)]: # "Update documentation on viewful vs viewless timeseries collections."

Viewless timeseries collections are rewritten to be on the buckets themselves, including setting the
`rawData` parameter to `true`, before routing the query as before. The rewrites that are applied are
in [query/timeseries/README](../query/timeseries/README.md).

## DDL operations

Users run DDL operations (`collMod`, `createIndexes`, `listIndexes`, `dropIndexes`, and etc...) on the
**view** namespace. The buckets collection is meant to be invisible to the end user: special permissions are
required to run DDL operations directly on it. The DDL coordinator translates the operation to the
buckets namespace using the function `setBucketNss`, stores it in the `ShardingDDLCoordinatorMetadata`,
and sets the `isTimeseriesNamespace` flag. Specific DDL coordinators will do further time-series rewrites
as necessary. For example, the `CreateCollectionCoordinator` will check for the presence of `timeseriesFields`
in the `ChunkManager` to decide if the shard key needs to be rewritten before forwarding the request to the shards.

When the shards receive the DDL operation, the shards will decide if the operation body needs to be
translated to the buckets namespace. For example, `listIndexes` checks if the `isTimeseriesNamespace`
flag is set to return all of the indexes on the buckets collection.

Additionally, there are operations that must perform a "reverse" translation (from buckets to the view).
`listIndexes` will return the existing indexes (created by `shardCollection/createIndexes`) after translating the index
from the buckets collection to the time-series view. This is because the buckets collection should be
invisible to the end-user.

## Sharding administrative commands

All sharding admin commands, such as `split` and `moveChunk` must be run on the **buckets** collection
directly. These are some of the only commands that users run on the **buckets** namespace, and not the **view** namespace.

## Orphan buckets and the bucket catalog

Open time-series buckets are stored in memory in the `BucketCatalog`. Incoming measurements will be
inserted into the open buckets in the catalog. If a chunk migration occurs, and a bucket becomes an
orphan on a specific shard, the `BucketCatalog` cannot insert any new measurements into these newly
orphaned buckets. Therefore, the bucket catalog must consider if buckets are orphaned. To achieve this,
after a chunk migration has succeeded, the `BucketCatalog` is cleared.
