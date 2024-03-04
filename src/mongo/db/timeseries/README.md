# Time-Series Collections

MongoDB supports a new collection type for storing time-series data with the [timeseries](../commands/create.idl)
collection option. A time-series collection presents a simple interface for inserting and querying
measurements while organizing the actual data in buckets.

A minimally configured time-series collection is defined by providing the [timeField](timeseries.idl)
at creation. Optionally, a meta-data field may also be specified to help group
measurements in the buckets. MongoDB also supports an expiration mechanism on measurements through
the `expireAfterSeconds` option.

A time-series collection `mytscoll` in the `mydb` database is represented in the [catalog](../catalog/README.md) by a
combination of a view and a system collection:

-   The view `mydb.mytscoll` is defined with the bucket collection as the source collection with
    certain properties:
    _ Writes (inserts only) are allowed on the view. Every document inserted must contain a time field.
    _ Querying the view implicitly unwinds the data in the underlying bucket collection to return
    documents in their original non-bucketed form. \* The aggregation stage [$\_internalUnpackBucket](../pipeline/document_source_internal_unpack_bucket.h) is used to
    unwind the bucket data for the view. For more information about this stage and query rewrites for
    time-series collections see [query/timeseries/README](../query/timeseries/README.md).
-   The system collection has the namespace `mydb.system.buckets.mytscoll` and is where the actual
    data is stored.
    -   Each document in the bucket collection represents a set of time-series data within a period of time.
    -   If a meta-data field is defined at creation time, this will be used to organize the buckets so that
        all measurements within a bucket have a common meta-data value.
    -   Besides the time range, buckets are also constrained by the total number and size of measurements.

Time-series collections can also be sharded. For more information about sharding-specific implementation
details, see [db/s/README_timeseries.md](../s/README_timeseries.md).

## Bucket Collection Schema

Uncompressed bucket (version 1):

```
{
    _id: <Object ID with time component equal to control.min.<time field>>,
    control: {
        // <Some statistics on the measurements such as min/max values of data fields>
        version: 1,  // Version of bucket schema, version 1 indicates the bucket is uncompressed.
        min: {
            <time field>: <time of first measurement in this bucket, rounded down based on granularity>,
            <field0>: <minimum value of 'field0' across all measurements>,
            <field1>: <maximum value of 'field1' across all measurements>,
            ...
        },
        max: {
            <time field>: <time of last measurement in this bucket>,
            <field0>: <maximum value of 'field0' across all measurements>,
            <field1>: <maximum value of 'field1' across all measurements>,
            ...
        },
        closed: <bool> // Optional, signals the database that this document will not receive any
                       // additional measurements.
    },
    meta: <meta-data field (if specified at creation) value common to all measurements in this bucket>,
    data: {
        <time field>: {
            '0': <time of first measurement>,
            '1': <time of second measurement>,
            ...
            '<n-1>': <time of n-th measurement>,
        },
        <field0>: {
            '0': <value of 'field0' in first measurement>,
            '1': <value of 'field0' in second measurement>,
            ...
        },
        <field1>: {
            '0': <value of 'field1' in first measurement>,
            '1': <value of 'field1' in second measurement>,
            ...
        },
        ...
    }
}
```

There are two types of compressed buckets, version 2 and version 3. They differ only in that the
entries in the data field of version 2 buckets are sorted on the time field, whereas this is not
enforced for version 3 buckets.

Compressed bucket (version 2 and version 3):

```
{
    _id: <Object ID with time component equal to control.min.<time field>>,
    control: {
        // <Some statistics on the measurements such as min/max values of data fields>
        version: 2,  // Version of bucket schema, version 2 indicates the bucket is compressed.
        min: {
            <time field>: <time of first measurement in this bucket, rounded down based on granularity>,
            <field0>: <minimum value of 'field0' across all measurements>,
            <field1>: <maximum value of 'field1' across all measurements>,
            ...
        },
        max: {
            <time field>: <time of last measurement in this bucket>,
            <field0>: <maximum value of 'field0' across all measurements>,
            <field1>: <maximum value of 'field1' across all measurements>,
            ...
        },
        closed: <bool>, // Optional, signals the database that this document will not receive any
                        // additional measurements.
        count: <int>    // The number of measurements contained in this bucket. Only present in
                        // compressed buckets.
    },
    meta: <meta-data field (if specified at creation) value common to all measurements in this bucket>,
    data: {
        <time field>: BinData(7, ...), // BinDataType 7 represents BSONColumn.
        <field0>:     BinData(7, ...),
        <field1>:     BinData(7, ...),
        ...
    }
}
```

### Bucket Versions

The versions that a bucket can take are V1, V2, and V3.

V1 buckets are uncompressed and their measurements are not sorted on time, V2 buckets are compressed
and have their measurements sorted on time, and V3 buckets are compressed but do not have their
measurements sorted on time. When we say that a bucket is compressed, we mean that for each field
in its data, the measurements for that field are stored in a BSONColumn [(More about BSONColumn and the binary data type, BinData7)](https://github.com/mongodb/mongo/blob/4e8319347d8ee243fa96fe186abd91bd6b4bbeb8/src/mongo/db/timeseries/README.md#bucket-collection-schema).

Starting in 8.0, newly created buckets will be V2 by default. V2 and V3 buckets in the BucketCatalog maintain
a BSONColumnBuilder for each data field. These builders are append-only, meaning that new measurements can only
be added to the end of the builder." If measurements come out of order by time into the
same bucket (which is more likely to happen during low cardinality concurrent bulk loads), we will promote
a V2 bucket to a V3 bucket. The bucket will behave the same way, the only difference being that
the measurements in a V3 bucket are not guaranteed to be in-order on time (and in fact, must have
at least one out-of-order measurement). Queries can also be less performant on V3 buckets, since they
cannot rely on the fact that V3 buckets have their measurements in order by time.

New V1 buckets will no longer be created in 8.0+, but existing V1 buckets from upgrades will
continue to be supported. Closed V1 buckets can be re-opened, and will be compressed when more
measurements are inserted into them.

### BSONColumnBuilder

Each V2 and V3 bucket has a InsertionOrderedColumnMap, which is a map from each data field (excluding the meta field) in a bucket
to a corresponding BSONColumnBuilder for that field. For example, if a bucket has a timefield `time`,
there will be a mapping (`time` -> BSONColumnnBuilder for the BSONColumn of time data).

BSONColumnBuilder stores binary data that can represent either the entire BSONColumn for a field or a partial
BSONColumn. When we are adding the measurements for a new field, its binary data will represent the entire BSONColumn
for that field. When we are appending measurements to existing fields, it will instead store the binary data
representing a partial BSONColumn which only includes the new measurements that have been added - the binary data difference
representing these new measurements can be retrieved by calling `intermediate()`. This binary data difference is used to generate
a DocDiff for the BSONColumn.

### DocDiff Support for BSONColumn

In order to avoid replicating entire compressed bucket documents, we take advantage of the fact that
adding elements to the BSONColumn is append-only and utilize DocDiff. Since with each addition only
the last few bytes of a BSONColumn binary change, we construct a DocDiff that includes information about
what the new binary data to be added is as well as at what offset to copy it into, for each field.
This approach works better in terms of oplog size and oplog entry size as the write batch size increases.

The DocDiff will take the form below:

```
{
    b(inary): {
        <field1>: {
            o(ffset): Number,    // Offset into existing BSONColumn
            d(ata):   BinData    // Binary data to copy to existing BSONColumn
        },
        ...,
        <fieldN>: {
            o(ffset): Number,    // Offset into existing BSONColumn
            d(ata):   BinData    // Binary data to copy to existing BSONColumn
        }
    }
}

```

### Metadata Normalization

If the value of a document's `metaField` field contains any object data, that data will be
_normalized_. The fields in each nested subobject will be sorted in lexicographic order.

This is done because many application languages offer support (and even default to) using unordered
dictionaries to store the fields in an object, resulting in randomized field order for inserts that
logically belong to the same time series. Normalizing this data allows us to bucket these documents
together efficiently.

As an example, consider a time series collection with a configured `{metaField: 'm'}`. Given a
document `{m: {c: 1, b: {f: true, d: 0}}}`, we will normalize this to
`{m: {b: {d: 0, f: true}, c: 1}}`.

This normalization occurs prior to routing documents when inserting in a sharded collection.

This normalization is not performed on `$match` expressions or in other places in the query system.
This may cause queries using object equality filters not to find all documents that one might
expect, as object equality is field-order sensitive. Due to this field-order sensitivity, object
equality filters are generally not recommended, and doubly-so for time series `metaField` data.
Queries should instead match on specific nested fields.

## Indexes

In order to support queries on the time-series collection that could benefit from indexed access
rather than collection scans, indexes may be created on the time, meta-data, and meta-data subfields
of a time-series collection. Starting in v6.0, indexes on time-series collection measurement fields
are permitted. The index key specification provided by the user via `createIndex` will be converted
to the underlying buckets collection's schema.

-   The details for mapping the index specification between the time-series collection and the
    underlying buckets collection may be found in
    [timeseries_index_schema_conversion_functions.h](timeseries_index_schema_conversion_functions.h).
-   Newly supported index types in v6.0 and up
    [store the original user index definition](https://github.com/mongodb/mongo/blob/cf80c11bc5308d9b889ed61c1a3eeb821839df56/src/mongo/db/timeseries/timeseries_commands_conversion_helper.cpp#L140-L147)
    on the transformed index definition. When mapping the bucket collection index to the time-series
    collection index, the original user index definition is returned.

Once the indexes have been created, they can be inspected through the `listIndexes` command or the
`$indexStats` aggregation stage. `listIndexes` and `$indexStats` against a time-series collection
will internally convert the underlying buckets collections' indexes and return time-series schema
indexes. For example, a `{meta: 1}` index on the underlying buckets collection will appear as
`{mm: 1}` when we run `listIndexes` on a time-series collection defined with `mm` for the meta-data
field.

`dropIndex` and `collMod` (`hidden: <bool>`, `expireAfterSeconds: <num>`) are also supported on
time-series collections.

Supported index types on the time field:

-   [Single](https://docs.mongodb.com/manual/core/index-single/).
-   [Compound](https://docs.mongodb.com/manual/core/index-compound/).
-   [Hashed](https://docs.mongodb.com/manual/core/index-hashed/).
-   [Wildcard](https://docs.mongodb.com/manual/core/index-wildcard/).
-   [Sparse](https://docs.mongodb.com/manual/core/index-sparse/).
-   [Multikey](https://docs.mongodb.com/manual/core/index-multikey/).
-   [Indexes with collations](https://docs.mongodb.com/manual/indexes/#indexes-and-collation).

Supported index types on the metaField or its subfields:

-   All of the supported index types on the time field.
-   [2d](https://docs.mongodb.com/manual/core/2d/) from v6.0.
-   [2dsphere](https://docs.mongodb.com/manual/core/2dsphere/) from v6.0.
-   [Partial](https://docs.mongodb.com/manual/core/index-partial/) from v6.0.

Supported index types on measurement fields in v6.0 and up only:

-   [Single](https://docs.mongodb.com/manual/core/index-single/) from v6.0.
-   [Compound](https://docs.mongodb.com/manual/core/index-compound/) from v6.0.
-   [2dsphere](https://docs.mongodb.com/manual/core/2dsphere/) from v6.0.
-   [Partial](https://docs.mongodb.com/manual/core/index-partial/) from v6.0.
-   [TTL](https://docs.mongodb.com/manual/core/index-ttl/) from v6.3. Must be used in conjunction with
    a `partialFilterExpression` based on the metaField or its subfields.

Index types that are not supported on time-series collections include
[unique](https://docs.mongodb.com/manual/core/index-unique/), and
[text](https://docs.mongodb.com/manual/core/index-text/).

## BucketCatalog

In order to facilitate efficient bucketing, we maintain the set of open buckets in the
`BucketCatalog` found in [bucket_catalog.h](bucket_catalog.h). At a high level, we attempt to group
writes from concurrent writers into batches which can be committed together to minimize the number
of underlying document writes. A writer will attempt to insert each document in its input batch to
the `BucketCatalog`, which will return either a handle to a `BucketCatalog::WriteBatch` or
information that can be used to retrieve a bucket from disk to reopen. A second attempt to insert
the document, potentially into the reopened bucket, should return a `BucketCatalog::WriteBatch`.
Upon finishing all of its inserts, the writer will check each write batch. If no other writer has
already claimed commit rights to a batch, it will claim the rights and commit the batch itself;
otherwise, it will set the batch aside to wait on later. When it has checked all batches, the writer
will wait on each remaining batch to be committed by another writer.

Internally, the `BucketCatalog` maintains a list of updates to each bucket document. When a batch
is committed, it will pivot the insertions into the column-format for the buckets as well as
determine any updates necessary for the `control` fields (e.g. `control.min` and `control.max`).

The first time a write batch is committed for a given bucket, the newly-formed document is
inserted. On subsequent batch commits, we perform an update operation. Instead of generating the
full document (a so-called "classic" update), we create a DocDiff directly (a "delta" or "v2"
update).

Any time a bucket document is updated without going through the `BucketCatalog`, the writer needs
to notify the `BucketCatalog` by calling `timeseries::handleDirectWrite` or `BucketCatalog::clear`
so that it can update its internal state and avoid writing any data which may corrupt the bucket
format.

### Bucket Reopening

If an initial attempt to insert a measurement finds no open bucket, or finds an open bucket that is
not suitable to house the incoming measurement, the `BucketCatalog` may return some information to
the caller that can be used to retrieve a bucket from disk to reopen. In some cases, this will be
the `_id` of an archived bucket (more details below). In other cases, this will be a set of filters
to use for a query. Once we retrieve the bucket from disk, we recreate the in-memory bucket
representation of the bucket.

The filters will include an exact match on the `metaField`, a range match on the `timeField`, size
filters on the `timeseriesBucketMaxCount` and `timeseriesBucketMaxSize` server parameters, and a
missing or `false` value for `control.closed`.

The query-based reopening path relies on a `{<metaField>: 1, <timeField>: 1}` index to execute
efficiently. This index is created by default for new time series collections created in v6.3+. If
the index does not exist, then query-based reopening will not be used.

When we reopen compressed buckets, in order to avoid fully decompressing and then fully re-compressing
the bucket we instantiate the bucket's BSONColumnBuilders from the existing BSONColumn binaries. Currently
this only supports scalar values; if the interleave mode (the mode where we are dealing with different types)
is detected in the input BSONColumn binary, we will fully decompress and re-compressed the bucket we are
reopening.

### Bucket Closure and Archival

A bucket is permanently closed by setting the optional `control.closed` flag, which makes it
ineligible for reopening. This can only be done manually for [Atlas Online Archive](https://www.mongodb.com/docs/atlas/online-archive/manage-online-archive/).

If the `BucketCatalog` is using more memory than it's given threshold (controlled by the server
parameter `timeseriesIdleBucketExpiryMemoryUsageThreshold`), it will start to archive or close idle
buckets. A bucket is considered idle if it is open and it does not have any uncommitted measurements
pending. Archiving a bucket removes most of its in-memory state from the `BucketCatalog`, but
retains a small record for quicker reopening in case we try to insert another measurement which
would fit in the archived bucket. If, after archiving all open idle buckets, the memory usage still
exceeds the limit, the `BucketCatalog` will remove archived bucket entries to reclaim more memory. A
bucket closed in this way remains eligible for query-based reopening.

The `BucketCatalog` will also close a bucket if a new measurement would cause the bucket to span a
greater amount of time between its oldest and newest timestamp than is allowed by the collection
settings. Such buckets remain eligible for reopening.

Finally, the `BucketCatalog` will archive a bucket if a new measurement's timestamp is earlier than
the minimum timestamp for the current open bucket for it's time series.

When a bucket is closed during insertion, the `BucketCatalog` will either open a new bucket or
reopen an old one in order to accommodate the new measurement.

## Bucketing Parameters

The maximum span of time that a single bucket is allowed to cover is controlled by
`bucketMaxSpanSeconds`.

When a new bucket is opened by the `BucketCatalog`, the timestamp component of its `_id`, and
equivalently the value of its `control.min.<time field>`, will be taken from the first measurement
inserted to the bucket and rounded down based on the `bucketRoundingSeconds`. This rounding will
generally be accomplished by basic modulus arithmetic operating on the number of seconds since the
epoch i.e. for an input timestamp `t` and a rounding value `r`, the rounded timestamp will be
taken as `t - (t % r)`.

A user may choose to set `bucketMaxSpanSeconds` and `bucketRoundingSeconds` directly when creating a
new collection in order to use "fixed bucketing". In this case we require that these two values are
equal to each other, strictly positive, and no more than 31536000 (365 days).

In most cases though, the user will instead want to use the `granularity` option. This option is
intended to convey the scale of the time between measurements in a given time-series, and encodes
some reasonable presets of "seconds", "minutes" and "hours". These presets correspond to
`bucketMaxSpanSeconds` values of 3600 (1 hour), 86400 (1 day), and 2592000 (30 days); and
`bucketRoundingSeconds` values of 60 (1 minute), 3600 (1 hour), and 86400 (1 day), respectively.

If the user does not specify any bucketing parameters when creating a collection, the default value
is `{granularity: "seconds"}`.

A `collMod` operation can change these settings as long as the net effect is that
`bucketMaxSpanSeconds` and `bucketRoundingSeconds` do not decrease, and the values remain in the
valid ranges. Notably, one can convert from fixed range bucketing to one of the `granularity`
presets or vice versa, as long as the associated seconds parameters do not decrease as a result.

## Updates and Deletes

Time-series collections support arbitrary updates and deletes with the same user facing behaviors as
regular collections.

### Deletes

Time-series user deletes are done one bucket document at a time. The bucket document will be
unpacked, with each of its measurements checked against the user delete query predicate. If there
are any measurements not needed to be deleted, they will be repacked back to the original bucket
document with the same `_id` as an update operation. If all measurements match the query predicate,
the whole bucket document will be deleted.

If the delete query predicate applies to the whole bucket document, the delete will skip unpacking
the bucket documents and directly run against the bucket documents.

### Updates

Similar to deletes, time-series user updates are done one bucket document at a time. The unmatched
measurements not needed to be updated will be repacked back to the original bucket document with the
same `_id` as an update operation. If any measurement matches the query predicate, the user-provided
update operation will be applied to the matched measurements, and the updated measurements will be
inserted into new bucket documents. If all measurements match the query predicate, the whole bucket
document will be deleted.

To avoid the [Halloween Problem](https://en.wikipedia.org/wiki/Halloween_Problem) for `{multi: true}`
updates, a [Spool Stage](https://github.com/mongodb/mongo/blob/cd7f99721a32a14bc76d20e207ebefd26134ff40/src/mongo/db/exec/spool.h)
is used to record all record ids of the bucket documents returned from the scan stage. This, along
with inserting updated measurements to new buckets, helps avoid seeing measurements already updated
by the current update command. If the query is non-selective and there are too many matching bucket
documents, the spool stage will spill record ids of matching bucket documents when it hits the
maximum memory usage limit.

Since time-series updates will perform at least two writes to storage (a modification to the
original bucket document and inserts of new bucket documents), the oplog entries are grouped
together as an `applyOps` command to ensure atomicity of the operation.

Time-series upserts share the same process as updates. An `_id` will be generated for the upserted
measurement.

### Transaction Support

Time-series singleton updates and deletes are supported in multi-document transactions. They are
used internally for singleton update/delete without shard key, shard key update, and retryable
time-series update/delete.

### Retryable Writes

Time-series deletes support retryable writes with the existing mechanisms. For time-series updates,
they are run through the Internal Transaction API to make sure the two writes to storage are atomic.

### Calculating Memory Usage

Memory usage for Buckets, the BucketCatalog, and other aspects of time-series collection internals (Stripes, BucketMetadata, etc)
is calculated using the [Timeseries Tracking Allocator](https://github.com/10gen/mongo/blob/f726b6db3a361122a87555dbea053d98b01685a3/src/mongo/db/timeseries/timeseries_tracking_allocator.h).

### Freezing Buckets

When bucket compression fails, we will fail the insert prompting the user to retry the write and "freeze"
the bucket that we failed to compress. Once a bucket is frozen, we will no longer attempt to write to it.

# References

See:
[MongoDB Blog: Time Series Data and MongoDB: Part 2 - Schema Design Best Practices](https://www.mongodb.com/blog/post/time-series-data-and-mongodb-part-2-schema-design-best-practices)

# Glossary

**bucket**: A group of measurements with the same meta-data over a limited period of time.

**bucket collection**: A system collection used for storing the buckets underlying a time-series
collection. Replication, sharding and indexing are all done at the level of buckets in the bucket
collection.

**measurement**: A set of related key-value pairs at a specific time.

**meta-data**: The key-value pairs of a time-series that rarely change over time and serve to
identify the time-series as a whole.

**time-series**: A sequence of measurements over a period of time.

**time-series collection**: A collection type representing a writable non-materialized view that
allows storing and querying a number of time-series, each with different meta-data.
