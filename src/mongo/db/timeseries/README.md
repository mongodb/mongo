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
* The view `mydb.mytscoll` is defined with the bucket collection as the source collection with
certain properties:
    * Writes (inserts only) are allowed on the view. Every document inserted must contain a time field.
    * Querying the view implicitly unwinds the data in the underlying bucket collection to return
      documents in their original non-bucketed form.
        * The aggregation stage [$_internalUnpackBucket](../pipeline/document_source_internal_unpack_bucket.h) is used to
          unwind the bucket data for the view.
* The system collection has the namespace `mydb.system.buckets.mytscoll` and is where the actual
  data is stored.
    * Each document in the bucket collection represents a set of time-series data within a period of time.
    * If a meta-data field is defined at creation time, this will be used to organize the buckets so that
      all measurements within a bucket have a common meta-data value.
    * Besides the time range, buckets are also constrained by the total number and size of measurements.

## Bucket Collection Schema

```
{
    _id: <Object ID with time component equal to first measurement in this bucket>,
    control: {
        // <Some statistics on the measurements such min/max values of data fields>
        version: 1,  // Version of bucket schema. Currently fixed at 1 since this is the
                     // first iteration of time-series collections.
        min: {
            <time field>: <time of first measurement in this bucket>,
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
            '0', <time of first measurement>,
            '1', <time of second measurement>,
            ...
            '<n-1>': <time of n-th measurement>,
        },
        <field0>: {
            '0', <value of 'field0' in first measurement>,
            '1', <value of 'field0' in first measurement>,
            ...
        },
        <field1>: {
            '0', <value of 'field1' in first measurement>,
            '1', <value of 'field1' in first measurement>,
            ...
        },
        ...
    }
}
```

## Indexes

In order to support queries on the time-series collection that could benefit from indexed access
rather than collection scans, indexes may be created on the time, meta-data, and meta-data subfields
of a time-series collection. The index key specification provided by the user via `createIndex` will
be converted to the underlying buckets collection's schema.
* The details for mapping the index specificiation between the time-series collection and the
  underlying buckets collection may be found in
  [timeseries_index_schema_conversion_functions.h](timeseries_index_schema_conversion_functions.h).

Once the indexes have been created, they can be inspected through the `listIndexes` command or the
`$indexStats` aggregation stage. `listIndexes` and `$indexStats` against a time-series collection
will internally convert the underlying buckets collections' indexes and return time-series schema
indexes. For example, a `{meta: 1}` index on the underlying buckets collection will appear as
`{mm: 1}` when we run `listIndexes` on a time-series collection defined with `mm` for the meta-data
field.

`dropIndex` and `collMod` (`hidden: <bool>`, `expireAfterSeconds: <num>`) are also supported on
time-series collections.

Most index types are supported on time-series collections, including
[hashed](https://docs.mongodb.com/manual/core/index-hashed/),
[wildcard](https://docs.mongodb.com/manual/core/index-wildcard/),
[sparse](https://docs.mongodb.com/manual/core/index-sparse/),
[multikey](https://docs.mongodb.com/manual/core/index-multikey/), and
[indexes with collations](https://docs.mongodb.com/manual/indexes/#indexes-and-collation).

Index types that are not supported on time-series collections include
[geo](https://docs.mongodb.com/manual/core/2dsphere/),
[partial](https://docs.mongodb.com/manual/core/index-partial/),
[unique](https://docs.mongodb.com/manual/core/index-unique/), and
[text](https://docs.mongodb.com/manual/core/index-text/).

## BucketCatalog

In order to facilitate efficient bucketing, we maintain the set of open buckets in the
`BucketCatalog` found in [bucket_catalog.h](bucket_catalog.h). At a high level, we attempt to group
writes from concurrent writers into batches which can be committed together to minimize the number
of underlying document writes. A writer will insert each document in its input batch to the
`BucketCatalog`, which will return a handle to a `BucketCatalog::WriteBatch`. Upon finishing its
inserts, the writer will check each write batch. If no other writer has already claimed commit
rights to a batch, it will claim the rights and commit the batch itself; otherwise, it will set the
batch aside to wait on later. When it has checked all batches, the writer will wait on each
remaining batch to be committed by another writer.

Internally, the `BucketCatalog` maintains a list of updates to each bucket document. When a batch
is committed, it will pivot the insertions into the column-format for the buckets as well as
determine any updates necessary for the `control` fields (e.g. `control.min` and `control.max`).

Any time a bucket document is updated without going through the `BucketCatalog`, the writer needs
to call `BucketCatalog::clear` for the document or namespace in question so that it can update its
internal state and avoid writing any data which may corrupt the bucket format. This is typically
handled by an op observer, but may be necessary to call from other places.

A bucket is closed either manually, by setting the optional `control.closed` flag, or automatically
by the `BucketCatalog` in a number of situations. If the `BucketCatalog` is using more memory than
it's given threshold (controlled by the server paramter
`timeseriesIdleBucketExpiryMemoryUsageThreshold`), it will start to close idle buckets. A bucket is
considered idle if it is open and it does not have any uncommitted measurements pending. The
`BucketCatalog` will also close a bucket if it contains more than the maximum number of measurments
(`timeseriesBucketMaxCount`), if it contains more than the maximum amount of data
(`timeseriesBucketMaxSize`), or if a new measurement would cause the bucket to span a greater
amount of time between it's oldest and newest time stamp than is allowed (currently hard-coded to
one hour).

The first time a write batch is committed for a given bucket, the newly-formed document is
inserted. On subsequent batch commits, we perform an update operation. Instead of generating the
full document (a so-called "classic" update), we create a DocDiff directly (a "delta" or "v2"
update).

# Updates and Deletes

Time-series collections support deletes which satisfy the following restrictions:
* Query on only the `metaField`
* `multi: true`

and updates which satisfy these same conditions, plus the following:
* Update only the `metaField`
* Update specified as an update document (versus a replacement document or update pipeline)
* `upsert: false`

These updates and deletes are performed by translating the operation into a corresponding update or
delete on the underlying buckets collection. In particular, for both the query and update document,
we replace any references to the collection's `metaField` with literal `"meta"` (see
[Bucket Collection Schema](#bucket-collection-schema)).

For example, for a time-series collection `db.ts` created with `metaField: "tag"`, consider an
update on this collection with query `{"tag.tag.a": "a"}` and update document
`{$set: {"tag.tag.a": "A"}, $rename: {"tag.tag.b": "tag.tag.c"}}`. This gets translated into an
update on `db.system.buckets.ts` with query `{"meta.tag.a": "a"}` and update document
`{$set: {"meta.tag.a": "A"}, $rename: {"meta.tag.b": "meta.tag.c"}}`. We can then execute this
translated update as a regular update operation. The same process applies for deletes.

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
