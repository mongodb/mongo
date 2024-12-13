# Catalog Internals

The catalog is where MongoDB stores information about the collections and indexes for a replica set
node. In some contexts we refer to this as metadata and to operations changing this metadata as
[DDL](#glossary) (Data Definition Language) operations. The catalog is persisted as a table with
BSON documents that each describe properties of a collection and its indexes. An in-memory catalog
caches this data for more efficient access.

The catalog provides a mapping from logical user-level namespaces to durable storage engine entities and provides a concurrency control layer to safely modify collections and indexes metadata for DDL operations.

See the [Storage Engine API](../storage/README.md) for relevant information.

## Durable Catalog

The durable catalog is persisted as a table with the `_mdb_catalog`
[ident](../storage/README.md#idents). Each entry in this table is indexed with a 64-bit `RecordId`,
referred to as the catalog ID, and contains a BSON document that describes the properties of a
collection and its indexes. The `DurableCatalog` class allows read and write access to the durable
data.

Starting in v5.2, catalog entries for time-series collections have a new flag called
`timeseriesBucketsMayHaveMixedSchemaData` in the `md` field. Time-series collections upgraded from
versions earlier than v5.2 may have mixed-schema data in buckets. This flag gets set to `true` as
part of the upgrade process and is removed as part of the downgrade process through the
[collMod command](https://github.com/mongodb/mongo/blob/cf80c11bc5308d9b889ed61c1a3eeb821839df56/src/mongo/db/catalog/coll_mod.cpp#L644-L663).

Starting in v7.1, catalog entries for time-series collections have a new flag called
`timeseriesBucketingParametersHaveChanged` in the `md` field.

**Example**: an entry in the durable catalog for a collection `test.employees` with an in-progress
index build on `{lastName: 1}`:

```
 {'ident': 'collection-0--2147780727179663754',
  'idxIdent': {'_id_': 'index-1--2147780727179663754',
               'lastName_1': 'index-2--2147780727179663754'},
  'md': {'indexes': [{'multikey': False,
                      'multikeyPaths': {'_id': Binary('\x00', 0)},
                      'ready': True,
                      'spec': {'key': {'_id': 1},
                               'name': '_id_',
                               'v': 2}},
                     {'multikey': False,
                      'multikeyPaths': {'_id': Binary('\x00', 0)},
                      'ready': False,
                      'buildUUID': UUID('d86e8657-1060-4efd-b891-0034d28c3078'),
                      'spec': {'key': {'lastName': 1},
                               'name': 'lastName_1',
                               'v': 2}}],
          'ns': 'test.employees',
          'options': {'uuid': UUID('795453e9-867b-4804-a432-43637f500cf7')},
          'timeseriesBucketsMayHaveMixedSchemaData': False,
          'timeseriesBucketingParametersHaveChanged': False},
  'ns': 'test.employees'}
```

### $listCatalog Aggregation Pipeline Operator

[$listCatalog](https://github.com/mongodb/mongo/blob/532c0679ef4fc8313a9e00a1334ca18e04ff6914/src/mongo/db/pipeline/document_source_list_catalog.h#L46) is an internal aggregation pipeline operator that may be used to inspect the contents
of the durable catalog on a running server. For catalog entries that refer to views, additional
information is retrieved from the enclosing database's `system.views` collection. The $listCatalog
is generally run on the admin database to obtain a complete view of the durable catalog, provided
the caller has the required
[administrative privileges](https://github.com/mongodb/mongo/blob/532c0679ef4fc8313a9e00a1334ca18e04ff6914/src/mongo/db/pipeline/document_source_list_catalog.cpp#L55).
Example command invocation and output from
[list_catalog.js](https://github.com/mongodb/mongo/blob/532c0679ef4fc8313a9e00a1334ca18e04ff6914/jstests/core/catalog/list_catalog.js#L98)):

```
> const adminDB = db.getSiblingDB('admin');
> adminDB.aggregate([{$listCatalog: {}}]);

Collection-less $listCatalog: [
    {
        "db" : "test",
        "name" : "system.views",
        "type" : "collection",
        "md" : {
            "ns" : "test.system.views",
            "options" : {
                "uuid" : UUID("a132c4ee-a1f4-4251-8eb2-c9f4afbeb9c1")
            },
            "indexes" : [
                {
                    "spec" : {
                        "v" : 2,
                        "key" : {
                            "_id" : 1
                        },
                        "name" : "_id_"
                    },
                    "ready" : true,
                    "multikey" : false,
                    "multikeyPaths" : {
                        "_id" : BinData(0,"AA==")
                    }
                }
            ]
        },
        "idxIdent" : {
            "_id_" : "index-6-2245557986372974053"
        },
        "ns" : "test.system.views",
        "ident" : "collection-5-2245557986372974053"
    },
    {
        "db" : "list_catalog",
        "name" : "simple",
        "type" : "collection",
        "md" : {
            "ns" : "list_catalog.simple",
            "options" : {
                "uuid" : UUID("a86445c2-3e3c-42ae-96be-5d451c977ed6")
            },
            "indexes" : [
                {
                    "spec" : {
                        "v" : 2,
                        "key" : {
                            "_id" : 1
                        },
                        "name" : "_id_"
                    },
                    "ready" : true,
                    "multikey" : false,
                    "multikeyPaths" : {
                        "_id" : BinData(0,"AA==")
                    }
                },
                {
                    "spec" : {
                        "v" : 2,
                        "key" : {
                            "a" : 1
                        },
                        "name" : "a_1"
                    },
                    "ready" : true,
                    "multikey" : false,
                    "multikeyPaths" : {
                        "a" : BinData(0,"AA==")
                    }
                }
            ]
        },
        "idxIdent" : {
            "_id_" : "index-62-2245557986372974053",
            "a_1" : "index-63-2245557986372974053"
        },
        "ns" : "list_catalog.simple",
        "ident" : "collection-61-2245557986372974053"
    },
    {
        "db" : "list_catalog",
        "name" : "system.views",
        "type" : "collection",
        "md" : {
            "ns" : "list_catalog.system.views",
            "options" : {
                "uuid" : UUID("2f76dd14-1d9a-42e1-8716-c1165cdbb00f")
            },
            "indexes" : [
                {
                    "spec" : {
                        "v" : 2,
                        "key" : {
                            "_id" : 1
                        },
                        "name" : "_id_"
                    },
                    "ready" : true,
                    "multikey" : false,
                    "multikeyPaths" : {
                        "_id" : BinData(0,"AA==")
                    }
                }
            ]
        },
        "idxIdent" : {
            "_id_" : "index-65-2245557986372974053"
        },
        "ns" : "list_catalog.system.views",
        "ident" : "collection-64-2245557986372974053"
    },
    {
        "db" : "list_catalog",
        "name" : "simple_view",
        "type" : "view",
        "ns" : "list_catalog.simple_view",
        "_id" : "list_catalog.simple_view",
        "viewOn" : "simple",
        "pipeline" : [
            {
                "$project" : {
                    "a" : 0
                }
            }
        ]
    },
    ...
]

```

The `$listCatalog` also supports running on a specific collection. See example in
[list_catalog.js](https://github.com/mongodb/mongo/blob/532c0679ef4fc8313a9e00a1334ca18e04ff6914/jstests/core/catalog/list_catalog.js#L77).

This aggregation pipeline operator is primarily intended for internal diagnostics and applications that require information not
currently provided by [listDatabases](https://www.mongodb.com/docs/v6.0/reference/command/listDatabases/),
[listCollections](https://www.mongodb.com/docs/v6.0/reference/command/listCollections/), and
[listIndexes](https://www.mongodb.com/docs/v6.0/reference/command/listIndexes/). The three commands referenced are part of the
[Stable API](https://www.mongodb.com/docs/v6.0/reference/stable-api/) with a well-defined output format (see
[listIndexes IDL](https://github.com/mongodb/mongo/blob/532c0679ef4fc8313a9e00a1334ca18e04ff6914/src/mongo/db/list_indexes.idl#L225)).

#### Read Concern Support

In terms of accessing the current state of databases, collections, and indexes in a running server,
the `$listCatalog` provides a consistent snapshot of the catalog in a single command invocation
using the default or user-provided
[read concern](https://www.mongodb.com/docs/v6.0/reference/method/db.collection.aggregate/). The
[list_catalog_read_concern.js](https://github.com/mongodb/mongo/blob/532c0679ef4fc8313a9e00a1334ca18e04ff6914/jstests/noPassthrough/list_catalog_read_concern.js#L46)
contains examples of using $listCatalog with a variety of read concern settings.

The traditional alternative would have involved a `listDatabases` command followed by a series of
`listCollections` and `listIndexes` calls, with the downside of reading the catalog at a different
point in time during each command invocation.

#### Examples of differences between listIndexes and $listCatalog results

The `$listCatalog` operator does not format its results with the IDL-derived formatters generated for the `listIndexes` command.
This has implications for applications that read the durable catalog using $listCatalog rather than the recommended listIndexes
command. Below are a few examples where the `listIndexes` results may differ from `$listCatalog`.

| Index Type                                                          | Index Option                                                                                                                                     | createIndexes                                                 | listIndexes                                                                               | $listCatalog                                                                                 |
| ------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------- | ----------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------- |
| [Sparse](https://www.mongodb.com/docs/v6.0/core/index-sparse/)      | [sparse (safeBool)](https://github.com/mongodb/mongo/blob/532c0679ef4fc8313a9e00a1334ca18e04ff6914/src/mongo/db/list_indexes.idl#L84)            | `db.t.createIndex({a: 1}, {sparse: 12345})`                   | `{ "v" : 2, "key" : { "a" : 1 }, "name" : "a_1", "sparse" : true }`                       | `{ "v" : 2, "key" : { "a" : 1 }, "name" : "a_1", "sparse" : 12345 }`                         |
| [TTL](https://www.mongodb.com/docs/v6.0/core/index-ttl/)            | [expireAfterSeconds (safeInt)](https://github.com/mongodb/mongo/blob/532c0679ef4fc8313a9e00a1334ca18e04ff6914/src/mongo/db/list_indexes.idl#L88) | `db.t.createIndex({created: 1}, {expireAfterSeconds: 10.23})` | `{ "v" : 2, "key" : { "created" : 1 }, "name" : "created_1", "expireAfterSeconds" : 10 }` | `{ "v" : 2, "key" : { "created" : 1 }, "name" : "created_1", "expireAfterSeconds" : 10.23 }` |
| [Geo](https://www.mongodb.com/docs/v6.0/tutorial/build-a-2d-index/) | [bits (safeInt)](https://github.com/mongodb/mongo/blob/532c0679ef4fc8313a9e00a1334ca18e04ff6914/src/mongo/db/list_indexes.idl#L117)              | `db.t.createIndex({p: '2d'}, {bits: 16.578})`                 | `{ "v" : 2, "key" : { "p" : "2d" }, "name" : "p_2d", "bits" : 16 }`                       | `{ "v" : 2, "key" : { "p" : "2d" }, "name" : "p_2d", "bits" : 16.578 }`                      |

#### $listCatalog in a sharded cluster

The `$listCatalog` operator supports running in a sharded cluster where the `$listCatalog`
result from each shard is combined at the router with additional identifying information similar
to the [shard](https://www.mongodb.com/docs/v6.0/reference/operator/aggregation/indexStats/#std-label-indexStats-output-shard) output field
of the `$indexStats` operator. The following is a sample test output from
[sharding/list_catalog.js](https://github.com/mongodb/mongo/blob/532c0679ef4fc8313a9e00a1334ca18e04ff6914/jstests/sharding/list_catalog.js#L40):

```
[
    {
        "db" : "list_catalog",
        "name" : "coll",
        "type" : "collection",
        "shard" : "list_catalog-rs0",
        "md" : {
            "ns" : "list_catalog.coll",
            "options" : {
                "uuid" : UUID("17886eb0-f157-45c9-9b63-efb8273f51da")
            },
            "indexes" : [
                {
                    "spec" : {
                        "v" : 2,
                        "key" : {
                            "_id" : 1
                        },
                        "name" : "_id_"
                    },
                    "ready" : true,
                    "multikey" : false,
                    "multikeyPaths" : {
                        "_id" : BinData(0,"AA==")
                    }
                }
            ]
        },
        "idxIdent" : {
            "_id_" : "index-56--2997668048670645427"
        },
        "ns" : "list_catalog.coll",
        "ident" : "collection-55--2997668048670645427"
    },
    {
        "db" : "list_catalog",
        "name" : "coll",
        "type" : "collection",
        "shard" : "list_catalog-rs1",
        "md" : {
            "ns" : "list_catalog.coll",
            "options" : {
                "uuid" : UUID("17886eb0-f157-45c9-9b63-efb8273f51da")
            },
            "indexes" : [
                {
                    "spec" : {
                        "v" : 2,
                        "key" : {
                            "_id" : 1
                        },
                        "name" : "_id_"
                    },
                    "ready" : true,
                    "multikey" : false,
                    "multikeyPaths" : {
                        "_id" : BinData(0,"AA==")
                    }
                }
            ]
        },
        "idxIdent" : {
            "_id_" : "index-55--2220352983339007214"
        },
        "ns" : "list_catalog.coll",
        "ident" : "collection-53--2220352983339007214"
    }
]

```

## Collection Catalog

The `CollectionCatalog` class holds in-memory state about all collections in all databases and is a
cache of the [durable catalog](#durable-catalog) state. It provides the following functionality:

- Register new `Collection` objects, taking ownership of them.
- Lookup `Collection` objects by their `UUID` or `NamespaceString`.
- Iterate over `Collection` objects in a database in `UUID` order.
- Deregister individual dropped `Collection` objects, releasing ownership.
- Allow closing/reopening the catalog while still providing limited `UUID` to `NamespaceString`
  lookup to support rollback to a point in time.
- Ensures `Collection` objects are in-sync with opened storage snapshots.

### Concurrency Control

See [Concurrency Control](../concurrency/README.md)

### Synchronization

Catalog access is synchronized using multi-version concurrency control where readers operate on
immutable catalog, collection and index instances. Writes use [copy-on-write][] to create newer
versions of the catalog, collection and index instances to be changed, contents are copied from the
previous latest version. Readers holding on to a catalog instance will thus not observe any writes
that happen after requesting an instance. If it is desired to observe writes while holding a catalog
instance then the reader must refresh it.

Catalog writes are handled with the `CollectionCatalog::write(callback)` interface. It provides the
necessary [copy-on-write][] abstractions. A writable catalog instance is created by making a
shallow copy of the existing catalog. The actual write is implemented in the supplied callback which
is allowed to throw. Execution of the write callbacks are serialized and may run on a different
thread than the thread calling `CollectionCatalog::write`. Users should take care of not performing
any blocking operations in these callbacks as it would block all other DDL writes in the system.

To avoid a bottleneck in the case the catalog contains a large number of collections (being slow to
copy), immutable data structures are used, concurrent writes are also batched together. Any thread
that enters `CollectionCatalog::write` while a catalog instance is being copied or while executing
write callbacks is enqueued. When the copy finishes, all enqueued write jobs are run on that catalog
instance by the copying thread.

### Collection objects

Objects of the `Collection` class provide access to a collection's properties between
[DDL](#glossary) operations that modify these properties. Modifications are synchronized using
[copy-on-write][]. Reads access immutable `Collection` instances. Writes, such as rename
collection, apply changes to a clone of the latest `Collection` instance and then atomically install
the new `Collection` instance in the catalog. It is possible for operations that read at different
points in time to use different `Collection` objects.

Notable properties of `Collection` objects are:

- catalog ID - to look up or change information from the DurableCatalog.
- UUID - Identifier that remains for the lifetime of the underlying MongoDb collection, even across
  DDL operations such as renames, and is consistent between different nodes and shards in a
  cluster.
- NamespaceString - The current name associated with the collection.
- Collation and validation properties.
- Decorations that are either `Collection` instance specific or shared between all `Collection`
  objects over the lifetime of the collection.

In addition `Collection` objects have shared ownership of:

- An [`IndexCatalog`](#index-catalog) - an in-memory structure representing the `md.indexes` data
  from the durable catalog.
- A `RecordStore` - an interface to access and manipulate the documents in the collection as stored
  by the storage engine.

A writable `Collection` may only be requested in an active [WriteUnitOfWork](#WriteUnitOfWork). The
new `Collection` instance is installed in the catalog when the storage transaction commits as the
first `onCommit` [Changes](../storage/README.md#changes) that run. This means that it is not allowed
to perform any modification to catalog, collection or index instances in `onCommit` handlers. Such
modifications would break the immutability property of these instances for readers. If the storage
transaction rolls back then the writable `Collection` object is simply discarded and no change is
ever made to the catalog.

A writable `Collection` is a clone of the existing `Collection`, members are either deep or
shallowed copied. Notably, a shallow copy is made for the [`IndexCatalog`](#index-catalog).

The oplog `Collection` follows special rules, it does not use [copy-on-write][] or any other form
of synchronization. Modifications operate directly on the instance installed in the catalog. It is
not allowed to read concurrently with writes on the oplog `Collection`.

Finally, there are two kinds of decorations on `Collection` objects. The `Collection` object derives
from `DecorableCopyable` and requires `Decoration`s to implement a copy-constructor. Collection
`Decoration`s are copied with the `Collection` when DDL operations occur. This is used for to keep
versioned query information per Collection instance. Additionally, there are
`SharedCollectionDecorations` for storing index usage statistics and query settings that are shared
between `Collection` instances across DDL operations.

### Collection lifetime

The `Collection` object is brought to existence in two ways:

1. Any DDL operation is run. Non-create operations such as `collMod` clone the existing `Collection`
   object.
2. Using an existing durable catalog entry to instantiate an existing collection. This happens when
   we:
   1. Load the `CollectionCatalog` during startup or after rollback.
   2. When we need to instantiate a collection at an earlier point-in-time because the `Collection`
      is not present in the `CollectionCatalog`, or the `Collection` is there, but incompatible with
      the snapshot. See [here](#catalog-changes-versioning-and-the-minimum-valid-snapshot) how a
      `Collection` is determined to be incompatible.
   3. When we read at latest concurrently with a DDL operation that is also performing multikey
      changes.

For (1) and (2.1) the `Collection` objects are stored as shared pointers in the `CollectionCatalog`
and available to all operations running in the database. These `Collection` objects are released
when the collection is dropped from the `CollectionCatalog` and there are no more operations holding
a shared pointer to the `Collection`. The can be during shutdown or when the `Collection` is dropped
and there are no more readers.

For (2.2) the `Collection` objects are stored as shared pointers in `OpenedCollections`, which is a
decoration on the `Snapshot`. Meaning these `Collection` objects are only available to the operation
that instantiated them. When the snapshot is abandoned, such as during query yield, these
`Collection` objects are released. Multiple lookups from the `CollectionCatalog` will re-use the
previously instantiated `Collection` instead of performing the instantiation at every lookup for the
same operation.

(2.3) is an edge case where neither latest or pending `Collection` match the opened snapshot due to
concurrent multikey changes.

Users of `Collection` instances have a few responsibilities to keep the object valid.

1. Hold a collection-level lock.
2. Use an AutoGetCollection helper.
3. Explicitly hold a reference to the `CollectionCatalog`.

### Index Catalog

Each `Collection` object owns an `IndexCatalog` object, which in turn has shared ownership of
`IndexCatalogEntry` objects that each again own an `IndexDescriptor` containing an in-memory
presentation of the data stored in the [durable catalog](#durable-catalog).

## Catalog Changes, versioning and the Minimum Valid Snapshot

Every catalog change has a corresponding write with a commit time. When registered `OpObserver`
objects observe catalog changes, they set the minimum valid snapshot of the `Collection` to the
commit timestamp. The `CollectionCatalog` uses this timestamp to determine whether the `Collection`
is valid for a given point-in-time read. If not, a new `Collection` instance will be instantiated to
satisfy the read.

When performing a DDL operation on a `Collection` object, the `CollectionCatalog` uses a two-phase
commit algorithm. In the first phase (pre-commit phase) the `Namespace` and `UUID` is marked as
pending commit. This has the affect of reading from durable storage to determine if the latest
instance or the pending commit instance in the catalog match. We return the instance that matches.
This is only for reads without a timestamp that come in during the pending commit state. The second
phase (commit phase) removes the `Namespace` and `UUID` from the pending commit state. With this, we
can guarantee that `Collection` objects are fully in-sync with the storage snapshot.

With lock-free reads there may be ongoing concurrent DDL operations. In order to have a
`CollectionCatalog` that's consistent with the snapshot, the following is performed when setting up
a lock-free read:

- Get the latest version of the `CollectionCatalog`.
- Open a snapshot.
- Get the latest version of the `CollectionCatalog` and check if it matches the one obtained
  earlier. If not, we need to retry this. Otherwise we'd have a `CollectionCatalog` that's
  inconsistent with the opened snapshot.

## Collection Catalog and Multi-document Transactions

- When we start the transaction we open a storage snapshot and stash a CollectionCatalog instance
  similar to a regular lock-free read (but holding the RSTL as opposed to lock-free reads).
- User reads within this transaction lock the namespace and ensures we have a Collection instance
  consistent with the snapshot (same as above).
- User writes do an additional step after locking to check if the collection instance obtained is
  the latest instance in the CollectionCatalog, if it is not we treat this as a WriteConflict so the
  transaction is retried.

The `CollectionCatalog` contains a mapping of `Namespace` and `UUID` to the `catalogId` for
timestamps back to the oldest timestamp. These are used for efficient lookups into the durable
catalog, and are resilient to create, drop and rename operations.

Operations that use collection locks (in any [lock mode](../concurrency/README.md#lock-modes)) can
rely on the catalog information of the collection not changing. However, when unlocking and then
re-locking, not only should operations recheck catalog information to ensure it is still valid, they
should also make sure to abandon the storage snapshot, so it is consistent with the in memory
catalog.

## Two-Phase Collection and Index Drop

Collections and indexes are dropped in two phases to ensure both that reads at points-in-time
earlier than the drop are still possible and startup recovery and rollback via a stable timestamp
can find the correct and expected data. The first phase removes the catalog entry associated with
the collection or index: this delete is versioned by the storage engine, so earlier PIT accesses
continue to see the catalog data. The second phase drops the collection or index data: this is not
versioned and there is no PIT access that can see it afterwards. WiredTiger versions document
writes, but not table drops: once a table is gone, the data is gone.

The first phase of drop clears the associated catalog entry, both in the in-memory catalog and in
the on-disk catalog, and then registers the collection or index's information with the reaper. No
new operations will find the collection or index in the catalog. Pre-existing operations with
references to the collection or index state may still be running and will retain their references
until they complete. The reaper receives the collection or index ident (identifier), along with a
reference to the in-memory collection or index state, and a drop timestamp.

The second phase of drop deletes the data. The reaper maintains a list of {ident, drop token, drop
timestamp} sets. A collection or index's data will be dropped when both the drop timestamp becomes
sufficiently persisted such that the catalog change will not be rolled back and no other reference
to the collection or index in-memory state (tracked via the drop token) remains. When no concurrent
readers of the collection or index are left, the drop token will be the only remaining reference to
the in-memory state. The drop timestamp must be both older than the timestamp of the last checkpoint
and the oldest_timestamp. Requiring the drop timestamp to reach the checkpoint time ensures that
startup recovery and rollback via recovery to a stable timestamp, which both recover to the last
checkpoint, will never be missing collection or index data that should still exist at the checkpoint
time that is less than the drop timestamp. Requiring the drop timestamp to pass (become older) than
the oldest_timestamp ensures that all reads, which are supported back to the oldest_timestamp,
successfully find the collection or index data.

There's a mechanism to delay an ident drop during the second phase via
`KVDropPendingIdentReaper::markIdentInUse()` when there are no more references to the drop token.
This is currently used when instantiating `Collection` objects at earlier points in time for reads,
and prevents the reaper from dropping the collection and index tables during the instantiation.

_Code spelunking starting points:_

- [_The KVDropPendingIdentReaper
  class_](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/kv/kv_drop_pending_ident_reaper.h)
  - Handles the second phase of collection/index drop. Runs when notified.
- [_The TimestampMonitor and TimestampListener
  classes_](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/storage_engine_impl.h#L178-L313)
  - The TimestampMonitor starts a periodic job to notify the reaper of the latest timestamp that is
    okay to reap.
- [_Code that signals the reaper with a
  timestamp_](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/storage_engine_impl.cpp#L932-L949)

# Read Operations

External reads via the find, count, distinct, aggregation, and mapReduce cmds do not take collection
MODE_IS locks (mapReduce does continue to take MODE_IX collection locks for writes). Internal
operations continue to take collection locks. Lock-free reads (only take the global lock in MODE_IS)
achieve this by establishing consistent in-memory and storage engine on-disk state at the start of
their operations. Lock-free reads explicitly open a storage transaction while setting up consistent
in-memory and on-disk read state. Internal reads with collection level locks implicitly start
storage transactions later via the MongoDB integration layer on the first attempt to read from a
collection or index. Unless a read operation is part of a larger write operation, the transaction
is rolled-back automatically when the last GlobalLock is released, explicitly during query yielding,
or from a call to abandonSnapshot(). Lock-free read operations must re-establish consistent state
after a query yield, just as at the start of a read operation.

See
[~GlobalLock](https://github.com/mongodb/mongo/blob/r4.4.0-rc13/src/mongo/db/concurrency/d_concurrency.h#L228-L239),
[PlanYieldPolicy::\_yieldAllLocks()](https://github.com/mongodb/mongo/blob/r4.4.0-rc13/src/mongo/db/query/plan_yield_policy.cpp#L182),
[RecoveryUnit::abandonSnapshot()](https://github.com/mongodb/mongo/blob/r4.4.0-rc13/src/mongo/db/storage/recovery_unit.h#L217).

## Collection Reads

Collection reads act directly on a
[RecordStore](https://github.com/mongodb/mongo/blob/r4.4.0-rc13/src/mongo/db/storage/record_store.h#L202)
or
[RecordCursor](https://github.com/mongodb/mongo/blob/r4.4.0-rc13/src/mongo/db/storage/record_store.h#L102).
The Collection object also provides [higher-level
accessors](https://github.com/mongodb/mongo/blob/r4.4.0-rc13/src/mongo/db/catalog/collection.h#L279)
to the RecordStore.

## Index Reads

Index reads act directly on a
[SortedDataInterface::Cursor](https://github.com/mongodb/mongo/blob/r4.4.0-rc13/src/mongo/db/storage/sorted_data_interface.h#L214).
Most readers create cursors rather than interacting with indexes through the
[IndexAccessMethod](https://github.com/mongodb/mongo/blob/r4.4.0-rc13/src/mongo/db/index/index_access_method.h#L142).

## Read Locks

### Locked Reads

The
[`AutoGetCollectionForRead`](https://github.com/mongodb/mongo/blob/58283ca178782c4d1c4a4d2acd4313f6f6f86fd5/src/mongo/db/db_raii.cpp#L89)
(`AGCFR`) RAII type is used by most client read operations. In addition to acquiring all necessary
locks in the hierarchy, it ensures that operations reading at points in time are respecting the
visibility rules of collection data and metadata.

`AGCFR` ensures that operations reading at a timestamp do not read at times later than metadata
changes on the collection (see
[here](https://github.com/mongodb/mongo/blob/58283ca178782c4d1c4a4d2acd4313f6f6f86fd5/src/mongo/db/db_raii.cpp#L158)).

### Lock-Free Reads (global MODE_IS lock only)

Lock-free reads use the
[`AutoGetCollectionForReadLockFree`](https://github.com/mongodb/mongo/blob/4363473d75cab2a487c6a6066b601d52230c7e1a/src/mongo/db/db_raii.cpp#L429)
helper, which, in addition to the logic of `AutoGetCollectionForRead`, will establish consistent
in-memory and on-disk catalog state and data view. Locks are avoided by comparing in-memory fetched
state before and after an on-disk storage snapshot is opened. If the in-memory state differs before
and after, then the storage snapshot is abandoned and the code will retry until before and after
match. Lock-free reads skip collection and RSTL locks, so
[the repl mode/state and collection state](https://github.com/mongodb/mongo/blob/4363473d75cab2a487c6a6066b601d52230c7e1a/src/mongo/db/db_raii.cpp#L97-L102)
are compared before and after. In general, lock-free reads work by acquiring all the 'versioned'
state needed for the read at the beginning, rather than relying on a collection-level lock to keep
the state from changing.

When setting up the read, `AutoGetCollectionForRead` will trigger the instantiation of a
`Collection` object when either:

- Reading at an earlier time than the minimum valid snapshot of the matching `Collection` from the `CollectionCatalog`.
- No matching `Collection` is found in the `CollectionCatalog`.

In versions earlier than v7.0 this would error with `SnapshotUnavailable`.

Sharding `shardVersion` checks still occur in the appropriate query plan stage code when/if the
shard filtering metadata is acquired. The `shardVersion` check after
`AutoGetCollectionForReadLockFree` sets up combined with a read request's `shardVersion` provided
before `AutoGetCollectionForReadLockFree` runs is effectively a before and after comparison around
the 'versioned' state setup. The sharding protocol obviates any special changes for lock-free
reads other than consistently using the same view of the sharding metadata
[(acquiring it once and then passing it where needed in the read code)](https://github.com/mongodb/mongo/blob/9e1f0ea4f371a8101f96c84d2ecd3811d68cafb6/src/mongo/db/catalog_raii.cpp#L251-L273).

### Consistent Data View with Operations Running Nested Lock-Free Reads

Currently commands that support lock-free reads are `find`, `count`, `distinct`, `aggregate` and
`mapReduce` --`mapReduce` still takes collection IX locks for its writes. These commands may use
nested `AutoGetCollection*LockFree` helpers in sub-operations. Therefore, the first lock-free lock
helper will establish a consistent in-memory and on-disk metadata and data view, and sub-operations
will use the higher level state rather than establishing their own. This is achieved by the first
lock helper fetching a complete immutable copy of the `CollectionCatalog` and saving it on the
`OperationContext`. Subsequent lock free helpers will check an `isLockFreeReadsOp()` flag on the
`OperationContext` and skip establishing their own state: instead, the `CollectionCatalog` saved on
the `OperationContext` will be accessed and no new storage snapshot will be opened. Saving a
complete copy of the entire in-memory catalog provides flexibility for sub-operations that may need
access to any collection.

_Code spelunking starting points:_

- [_AutoGetCollectionForReadLockFree preserves an immutable CollectionCatalog_](https://github.com/mongodb/mongo/blob/dcf844f384803441b5393664e500008fc6902346/src/mongo/db/db_raii.cpp#L141)
- [_AutoGetCollectionForReadLockFree returns early if already running lock-free_](https://github.com/mongodb/mongo/blob/dcf844f384803441b5393664e500008fc6902346/src/mongo/db/db_raii.cpp#L108-L112)
- [_The lock-free operation flag on the OperationContext_](https://github.com/mongodb/mongo/blob/dcf844f384803441b5393664e500008fc6902346/src/mongo/db/operation_context.h#L298-L300)

## Secondary Reads

The oplog applier applies entries out-of-order to provide parallelism for data replication. This
exposes readers with no set read timestamp to the possibility of seeing inconsistent states of data.

Because of this, reads on secondaries are generally required to read at replication's
[lastApplied](../repl/README.md#replication-timestamp-glossary) optime instead (see
[SERVER-34192](https://jira.mongodb.org/browse/SERVER-34192)). LastApplied is used because on
secondaries it is only updated after each oplog batch, which is a known consistent state of data.

AGCFR provides the mechanism for secondary reads. This is implemented by switching the ReadSource of [eligible
readers](https://github.com/mongodb/mongo/blob/58283ca178782c4d1c4a4d2acd4313f6f6f86fd5/src/mongo/db/storage/snapshot_helper.cpp#L106)
to read at
[kLastApplied](https://github.com/mongodb/mongo/blob/58283ca178782c4d1c4a4d2acd4313f6f6f86fd5/src/mongo/db/storage/recovery_unit.h#L411).

# Write Operations

Operations that write to collections and indexes must take collection locks. Storage engines require
all operations to hold at least a collection IX lock to provide document-level concurrency.
Operations must perform writes in the scope of a [WriteUnitOfWork](../storage/README.md#writeunit).

## Collection and Index Writes

Collection write operations (inserts, updates, and deletes) perform storage engine writes to both
the collection's RecordStore and relevant index's SortedDataInterface in the same storage transaction, or WUOW. This ensures that completed, not-building indexes are always consistent with collection data.

## Vectored Inserts

To support efficient bulk inserts, we provide an internal API on collections, insertDocuments, that
supports 'vectored' inserts. Writers that wish to insert a vector of many documents will
bulk-allocate OpTimes for each document into a vector and pass both to insertDocument. In a single
WriteUnitOfWork, for every document, a commit timestamp is set with the OpTime, and the document is
inserted. The storage engine allows each document to appear committed at a different timestamp, even
though all writes took place in a single storage transaction.

See
[insertDocuments](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ops/write_ops_exec.cpp#L315)
and

# Indexes

An index is a storage engine data structure that provides efficient lookup on fields in a
collection's data set. Indexes map document fields, keys, to documents such that a full collection
scan is not required when querying on a specific field.

All user collections have a unique index on the `_id` field, which is required. The oplog and some
system collections do not have an \_id index.

Also see [MongoDB Manual - Indexes](https://docs.mongodb.com/manual/indexes/).

## Index Constraints

### Unique indexes

A unique index maintains a constraint such that duplicate values are not allowed on the indexed
field(s).

To convert a regular index to unique, one has to follow the two-step process:

- The index has to be first set to `prepareUnique` state using `collMod` command with the index
  option `prepareUnique: true`. In this state, the index will start rejecting writes introducing
  duplicate keys.
- The `collMod` command with the index option `unique: true` will then check for the uniqueness
  constraint and finally update the index spec in the catalog under a collection `MODE_X` lock.

If the index already has duplicate keys, the conversion in step two will fail and return all
violating documents' ids grouped by the keys. Step two can be retried to finish the conversion after
all violating documents are fixed. Otherwise, the conversion can be cancelled using `collMod`
command with the index option `prepareUnique: false`.

The `collMod` option `dryRun: true` can also be specified to check for duplicates in the index
without attempting to actually convert it.

### Multikey Indexes

An index is considered "multikey" if there are multiple keys that map to the same record. That is,
there are indexed fields with array values. For example, with an index on `{a: 1}`, the document
`{a: [1, 2, 3]}` automatically makes the index multikey. If an index is flagged as multikey, queries
change behavior when reading from the index. It makes reads less efficient because queries can no
longer assume that after reading an index entry, no further entries will have the same key values.

When the first multikey document is inserted into an index, a `multikey: true` flag is set on the
index in the durable catalog entry for the collection. Since this catalog entry is a document shared
across the entire collection, allowing any writer to modify the catalog entry would result in
excessive WriteConflictExceptions for other writers.

To solve this problem, the multikey state is tracked in memory, and only persisted when it changes
to `true`. Once `true`, an index is always multikey.

See
[MultiKeyPaths](https://github.com/mongodb/mongo/blob/r4.4.0-rc9/src/mongo/db/index/multikey_paths.h#L57),
[IndexCatalogEntryImpl::setMultikey](https://github.com/mongodb/mongo/blob/r4.4.0-rc9/src/mongo/db/catalog/index_catalog_entry_impl.cpp#L184),
and [Multikey Indexes - MongoDB Manual](https://docs.mongodb.com/manual/core/index-multikey/).

# Index Builds

Indexes are built by performing a full scan of collection data. To be considered consistent, an
index must correctly map keys to all documents.

At a high level, omitting details that will be elaborated upon in further sections, index builds
have the following procedure:

- While holding a collection X lock, write a new index entry to the array of indexes included as
  part of a durable catalog entry. This entry has a `ready: false` component. See [Durable
  Catalog](#durable-catalog).
- Downgrade to a collection IX lock.
- Scan all documents on the collection to be indexed
  - Generate [KeyString](https://github.com/mongodb/mongo/tree/master/src/mongo/db/storage/keystring_README.md)
    keys for the indexed fields for each document
  - Periodically yield locks and storage engine snapshots
  - Insert the generated keys into the [external sorter](#the-external-sorter)
- Read the sorted keys from the external sorter and [bulk
  load](http://source.wiredtiger.com/3.2.1/tune_bulk_load.html) into the storage engine index.
  Bulk-loading requires keys to be inserted in sorted order, but builds a B-tree structure that is
  more efficiently filled than with random insertion.
- While holding a collection X lock, make a final `ready: true` write to the durable catalog.

## Hybrid Index Builds

Hybrid index builds refer to the default procedure introduced in 4.2 that produces efficient index
data structures without blocking reads or writes for extended periods of time. This is achieved by
performing a full collection scan and bulk-loading keys (described above) while concurrently
intercepting new writes into a temporary storage engine table.

### Temporary Side Table For New Writes

During an index build, new writes (i.e. inserts, updates, and deletes) are applied to the collection
as usual. However, instead of writing directly into the index table as a normal write would, index
keys for documents are generated and intercepted by inserting into a temporary _side-writes_ table.
Writes are intercepted for the duration of the index build, from before the collection scan begins
until the build is completed.

Both inserted and removed keys are recorded in the _side-writes_ table. For example, during an index
build on `{a: 1}`, an update on a document from `{_id: 0, a: 1}` to `{_id: 0, a: 2}` is recorded as
a deletion of the key `1` and an insertion of the key `2`.

Once the collection scan and bulk-load phases of the index build are complete, these intercepted
keys are applied directly to the index in three phases:

- While holding a collection IX lock to allow concurrent reads and writes
  - Because writes are still accepted, new keys may appear at the end of the _side-writes_ table.
    They will be applied in subsequent steps.
- While holding a collection S lock to block concurrent writes, but not reads
- While holding a collection X lock to block all reads and writes

See
[IndexBuildInterceptor::sideWrite](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/index/index_build_interceptor.cpp#L403)
and
[IndexBuildInterceptor::drainWritesIntoIndex](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/index/index_build_interceptor.cpp#L135).

### Temporary Table For Duplicate Key Violations

Unique indexes created with `{unique: true}` enforce a constraint that there are no duplicate keys
in an index. The hybrid index procedure makes it challenging to detect duplicates because keys are
split between the bulk-loaded index and the side-writes table. Additionally, during the lifetime of
an index build, concurrent writes may introduce and resolve duplicate key conflicts on the index.

For those reasons, during an index build we temporarily allow duplicate key violations, and record
any detected violations in a temporary table, the _duplicate key table_. At the conclusion of the
index build, under a collection X lock, [duplicate keys are
re-checked](https://github.com/mongodb/mongo/blob/r4.4.0-rc9/src/mongo/db/index_builds_coordinator.cpp#L2312).
If there are still constraint violations, an error is thrown.

See
[DuplicateKeyTracker](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/index/duplicate_key_tracker.h#L48).

### Temporary Table For Key Generation Errors

In addition to uniqueness constraints, indexes may have per-key constraints. For example, a compound
index may not be built on documents with parallel arrays. An index build on `{a: 1, b: 1}` will fail
to generate a key for `{a: [1, 2, 3], b: [4, 5, 6]}`.

On a primary under normal circumstances, we could fail an index build immediately after encountering
a key generation error. Since secondaries apply oplog entries [out of
order](../repl/README.md#oplog-entry-application), however, spurious key generation errors may be
encountered on otherwise consistent data. To solve this problem, we can relax key constraints and
suppress key generation errors on secondaries.

With the introduction of simultaneous index builds, an index build may be started on a secondary
node, but complete while it is a primary after a state transition. If we ignored constraints while
in the secondary state, we would not be able to commit the index build and guarantee its consistency
since we may have suppressed valid key generation errors.

To solve this problem, on both primaries and secondaries, the records associated with key generation
errors are skipped and recorded in a temporary table, the _skipped record table_. Like duplicate key
constraints, but only on primaries at the conclusion of the index build, the keys for the [skipped
records are
re-generated](https://github.com/mongodb/mongo/blob/r4.4.0-rc9/src/mongo/db/index_builds_coordinator.cpp#L2294)
and re-inserted under a collection X lock. If there are still constraint violations, an error is
thrown. Secondaries rely on the primary's decision to commit as assurance that skipped records do
not need to be checked.

See
[SkippedRecordTracker](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/index/skipped_record_tracker.h#L45).

## Replica Set Index Builds

Also referred to as "simultaneous index builds" and "two-phase index builds".

As of 4.4, index builds in a replica set use a two-phase commit protocol. When a primary starts an
index build, it spawns a background thread and replicates a `startIndexBuild` oplog entry. Secondary
nodes will start the index build in the background as soon as they apply that oplog entry. When a
primary is done with its indexing, it will decide to replicate either an `abortIndexBuild` or
`commitIndexBuild` oplog entry.

Simultaneous index builds are resilient to replica set state transitions. The node that starts an
index build does not need to be the same node that decides to commit it.

See [Index Builds in Replicated Environments - MongoDB
Manual](https://docs.mongodb.com/master/core/index-creation/#index-builds-in-replicated-environments).

Server 7.1 introduces the following improvements:

- Index builds abort immediately after detecting errors other than duplicate key
  violations. Before 7.1, index builds aborted the index build close to
  completion, potentially long after detection.
- A secondary member can abort a two-phase index build. Before 7.1, a secondary was forced
  to crash instead. See the [Voting for Abort](#voting-for-abort) section.
- Index builds are cancelled if there isn't enough storage space available. See the
  [Disk Space](#disk-space) section.

### Commit Quorum

The purpose of `commitQuorm` is to ensure secondaries are ready to commit an index build quickly.
This minimizes replication lag on secondaries: secondaries, on receipt of a `commitIndexBuild` oplog
entry, will stall oplog application until the local index build can be committed. `commitQuorum`
delays commit of an index build on the primary node until secondaries are also ready to commit. A
primary will not commit an index build until a minimum number of data-bearing nodes are ready to
commit the index build. Index builds can take anywhere from moments to days to complete, so the
replication lag can be very significant. Note: `commitQuorum` makes no guarantee that indexes on
secondaries are ready for use when the command completes, `writeConcern` must still be used for
that.

A `commitQuorum` option can be provided to the `createIndexes` command and specifies the number of
nodes, including itself, for which a primary must wait to be ready before committing. The `commitQuorum`
option accepts the same range of values as the writeConcern `"w"` option. This can be an integer
specifying the number of nodes, `"majority"`, `"votingMembers"`, or a replica set tag. The default value
is `"votingMembers"`, or all voting data-bearing nodes.

Nodes (both primary and secondary) submit votes to the primary when they have finished scanning all
data on a collection and performed the first drain of side-writes. Voting is implemented by a
`voteCommitIndexBuild` command, and is persisted as a write to the replicated
`config.system.indexBuilds` collection.

While waiting for a commit decision, primaries and secondaries continue receiving and applying new
side writes. When a quorum is reached, the current primary, under a collection X lock, will check
the remaining index constraints. If there are errors, it will replicate an `abortIndexBuild` oplog
entry. If the index build is successful, it will replicate a `commitIndexBuild` oplog entry.

Secondaries that were not included in the commit quorum and receive a `commitIndexBuild` oplog entry
will block replication until their index build is complete.

The `commitQuorum` for a running index build may be changed by the user via the
[`setIndexCommitQuorum`](https://github.com/mongodb/mongo/blob/v6.0/src/mongo/db/commands/set_index_commit_quorum_command.cpp#L55)
server command.

See
[IndexBuildsCoordinator::\_waitForNextIndexBuildActionAndCommit](https://github.com/mongodb/mongo/blob/r4.4.0-rc9/src/mongo/db/index_builds_coordinator_mongod.cpp#L632).

### Voting for Abort

As of 7.1, a secondary can abort a two-phase index build by sending a `voteAbortIndexBuild` signal
to the primary. In contrast, before 7.1 it was forced to crash. Common causes for aborting the index
build are a killOp on the index build or running low on storage space.
The primary, upon receiving a vote to abort the index build from a secondary, will replicate an
`abortIndexBuild` oplog entry. This will cause all secondaries to gracefully abort the index build,
even if a specific secondary had already voted to commit the index build.

Note that once a secondary has voted to commit the index build, it cannot retract the vote. In the
unlikely event that a secondary has voted for commit and for some reason it must abort while waiting
for the primary to replicate a `commitIndexBuild` oplog entry, the secondary is forced to crash.

### Disk Space

As of 7.1, an index build can abort due to a replica set member running low on disk space. This
applies both to primary and secondary nodes. Additionally, on a primary the index build won't start
if the available disk space is low.
The minimum amount of disk space is controlled by
[indexBuildMinAvailableDiskSpaceMB](https://github.com/mongodb/mongo/blob/406e69f6f5dee8b698c4e4308de2e9e5cef6c12c/src/mongo/db/storage/two_phase_index_build_knobs.idl#L71)
which defaults to 500MB.

## Resumable Index Builds

On clean shutdown, index builds save their progress in internal idents that will be used for resuming
the index builds when the server starts up. The persisted information includes:

- [Phase of the index build](https://github.com/mongodb/mongo/blob/0d45dd9d7ba9d3a1557217a998ad31c68a897d47/src/mongo/db/resumable_index_builds.idl#L43) when it was interrupted for shutdown:
  - initialized
  - collection scan
  - bulk load
  - drain writes
- Information relevant to the phase for reconstructing the internal state of the index build at
  startup. This may include:
  - The internal state of the external sorter.
  - Idents for side writes, duplicate keys, and skipped records.

During [startup recovery](#startup-recovery), the persisted information is used to reconstruct the
in-memory state for the index build and resume from the phase that we left off in. If we fail to
resume the index build for whatever reason, the index build will restart from the beginning.

Not all incomplete index builds are resumable upon restart. The current criteria for index build
resumability can be found in [IndexBuildsCoordinator::isIndexBuildResumable()](https://github.com/mongodb/mongo/blob/0d45dd9d7ba9d3a1557217a998ad31c68a897d47/src/mongo/db/index_builds_coordinator.cpp#L375). Generally,
index builds are resumable under the following conditions:

- Storage engine is configured to be persistent with encryption disabled.
- The index build is running on a voting member of the replica set with the default [commit quorum](#commit-quorum)
  `"votingMembers"`.
- Majority read concern is enabled.

The [Recover To A Timestamp (RTT) rollback algorithm](https://github.com/mongodb/mongo/blob/04b12743cbdcfea11b339e6ad21fc24dec8f6539/src/mongo/db/repl/README.md#rollback) supports
resuming index builds interrupted at any phase. On entering rollback, the resumable
index information is persisted to disk using the same mechanism as shutdown. We resume the
index build using the startup recovery logic that RTT uses to bring the node back to a writable
state.

For improved rollback semantics, resumable index builds require a majority read cursor during
collection scan phase. Index builds wait for the majority commit point to advance before starting
the collection scan. The majority wait happens after installing the
[side table for intercepting new writes](#temporary-side-table-for-new-writes).

See [MultiIndexBlock::\_constructStateObject()](https://github.com/mongodb/mongo/blob/0d45dd9d7ba9d3a1557217a998ad31c68a897d47/src/mongo/db/catalog/multi_index_block.cpp#L900)
for where we persist the relevant information necessary to resume the index build at shutdown
and [StorageEngineImpl::\_handleInternalIdents()](https://github.com/mongodb/mongo/blob/0d45dd9d7ba9d3a1557217a998ad31c68a897d47/src/mongo/db/storage/storage_engine_impl.cpp#L329)
for where we search for and parse the resume information on startup.

## Single-Phase Index Builds

Index builds on empty collections replicate a `createIndexes` oplog entry. This oplog entry was used
before FCV 4.4 for all index builds, but continues to be used in FCV 4.4 only for index builds that
are considered "single-phase" and do not need to run in the background. Unlike two-phase index
builds, the `createIndexes` oplog entry is always applied synchronously on secondaries during batch
application.

See [createIndexForApplyOps](https://github.com/mongodb/mongo/blob/6ea7d1923619b600ea0f16d7ea6e82369f288fd4/src/mongo/db/repl/oplog.cpp#L176-L183).

# The External Sorter

The external sorter is a MongoDB component that sorts large volumes of data, spilling in-memory data
to disk in order to bound its memory consumption as needed. It is used to sort documents read from
disk for the purposes of index creation and sorted query results. Index creation must read out all
of a collection's documents, generate index keys, sort them for the new index, and finally write the
sorted index keys out to the new index. A query with sorted results that do not match any index
ordering must read all the documents matching its specifications and then sort the results according
to the ordering specifications before returning the sorted results to the user.

The amount of data that is handled for both of these operations can be too large to keep in memory.
Therefore, the data is iteratively read from the storage engine, sorted, and written out to
temporary files whenever / if the maximum user adjustable memory limit is reached. Then the sorted
blocks of entries are iteratively read back into memory (if needed), following the coalescing phase
of the merge sort algorithm, and streamed to their final destination.

The maximum amount of memory allowed for an index build is controlled by the
`maxIndexBuildMemoryUsageMegabytes` server parameter. The sorter is passed this value and uses it to
regulate when to write a chunk of sorted data out to disk in a temporary file. The sorter keeps track
of the chunks of data spilled to disk using one Iterator for each spill. The memory needed for the iterators
is taken out of the `maxIndexBuildMemoryUsageMegabytes` and it is a percentage of `maxIndexBuildMemoryUsageMegabytes`
define by the `maxIteratorsMemoryUsagePercentage` server parameter with minimum value enough to store one
iterator and maximum value 1MB.

_Code spelunking starting points:_

- [_The External Sorter Classes_](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/sorter/sorter.h)

# The TTLMonitor

The TTLMonitor runs as a background job on each mongod. On a mongod primary, the TTLMonitor is responsible for removing documents expired on [TTL Indexes](https://www.mongodb.com/docs/manual/core/index-ttl/) across the mongod instance. It continuously runs in a loop that sleeps for ['ttlMonitorSleepSecs'](https://github.com/mongodb/mongo/blob/d88a892d5b18035bd0f5393a42690e705c2007d7/src/mongo/db/ttl.idl#L39) and then performs a TTL Pass to remove all expired documents.

The TTLMonitor exhibits different behavior pending on whether batched deletes are enabled. When enabled (the default), the TTLMonitor batches TTL deletions and also removes expired documents more fairly among TTL indexes. When disabled, the TTLMonitor falls back to legacy, doc-by-doc deletions and deletes all expired documents from a single TTL index before moving to the next one. The legacy behavior can lead to the TTLMonitor getting "stuck" deleting large ranges of documents on a single TTL index, starving other indexes of deletes at regular intervals.

### Fair TTL Deletion

If ['ttlMonitorBatchDeletes'](https://github.com/mongodb/mongo/blob/d88a892d5b18035bd0f5393a42690e705c2007d7/src/mongo/db/ttl.idl#L48) is specified, the TTLMonitor will batch deletes and provides fair TTL deletion as follows:

- The TTL pass consists of one or more subpasses.
- Each subpass refreshes its view of TTL indexes in the system. It removes documents on each TTL index in a round-robin fashion until there are no more expired documents or ['ttlMonitorSubPassTargetSecs'](https://github.com/mongodb/mongo/blob/d88a892d5b18035bd0f5393a42690e705c2007d7/src/mongo/db/ttl.idl#L58) is reached.
  - The delete on each TTL index removes up to ['ttlIndexDeleteTargetDocs'](https://github.com/mongodb/mongo/blob/d88a892d5b18035bd0f5393a42690e705c2007d7/src/mongo/db/ttl.idl#L84) or runs up to ['ttlIndexDeleteTargetTimeMS'](https://github.com/mongodb/mongo/blob/d88a892d5b18035bd0f5393a42690e705c2007d7/src/mongo/db/ttl.idl#L72), whichever target is met first. The same TTL index can be queued up to be revisited in the same subpass if there are outstanding deletions.
  - A TTL index is not visited any longer in a subpass once all documents are deleted.
- If there are outstanding deletions by the end of the subpass for any TTL index, a new subpass starts immediately within the same pass.

_Code spelunking starting points:_

- [_The TTLMonitor Class_](https://github.com/mongodb/mongo/blob/d88a892d5b18035bd0f5393a42690e705c2007d7/src/mongo/db/ttl.h)
- [_The TTLCollectionCache Class_](https://github.com/mongodb/mongo/blob/d88a892d5b18035bd0f5393a42690e705c2007d7/src/mongo/db/ttl_collection_cache.h)
- [_ttl.idl_](https://github.com/mongodb/mongo/blob/d88a892d5b18035bd0f5393a42690e705c2007d7/src/mongo/db/ttl.idl)

# Repair

Data corruption has a variety of causes, but can usually be attributed to misconfigured or
unreliable I/O subsystems that do not make data durable when called upon, often in the event of
power outages.

MongoDB provides a command-line `--repair` utility that attempts to recover as much data as possible
from an installation that fails to start up due to data corruption.

- [Types of Corruption](#types-of-corruption)
- [Repair Procedure](#repair-procedure)

## Types of Corruption

MongoDB repair attempts to address the following forms of corruption:

- Corrupt WiredTiger data files
  - Includes all collections, `_mdb_catalog`, and `sizeStorer`
- Missing WiredTiger data files
  - Includes all collections, `_mdb_catalog`, and `sizeStorer`
- Index inconsistencies
  - Validate [repair mode](#repair-mode) attempts to fix index inconsistencies to avoid a full index
    rebuild.
  - Indexes are rebuilt on collections after they have been salvaged or if they fail validation and
    validate repair mode is unable to fix all errors.
- Unsalvageable collection data files
- Corrupt metadata
  - `WiredTiger.wt`, `WiredTiger.turtle`, and WT journal files
- “Orphaned” data files
  - Collection files missing from the `WiredTiger.wt` metadata
  - Collection files missing from the `_mdb_catalog` table
  - We cannot support restoring orphaned files that are missing from both metadata sources
- Missing `featureCompatibilityVersion` document

## Repair Procedure

1. Initialize the WiredTigerKVEngine. If a call to `wiredtiger_open` returns the `WT_TRY_SALVAGE`
   error code, this indicates there is some form of corruption in the WiredTiger metadata. Attempt
   to [salvage the
   metadata](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L1046-L1071)
   by using the WiredTiger `salvage=true` configuration option.
2. Initialize the StorageEngine and [salvage the `_mdb_catalog` table, if
   needed](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/storage_engine_impl.cpp#L95).
3. Recover orphaned collections.
   - If an [ident](#glossary) is known to WiredTiger but is not present in the `_mdb_catalog`,
     [create a new
     collection](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/storage_engine_impl.cpp#L145-L189)
     with the prefix `local.orphan.<ident-name>` that references this ident.
   - If an ident is present in the `_mdb_catalog` but not known to WiredTiger, [attempt to recover
     the
     ident](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/storage_engine_impl.cpp#L197-L229).
     This [procedure for orphan
     recovery](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L1525-L1605)
     is a less reliable and more invasive. It involves moving the corrupt data file to a temporary
     file, creates a new table with the same name, replaces the original data file over the new
     one, and
     [salvages](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L1525-L1605)
     the table in attempt to reconstruct the table.
4. [Verify collection data
   files](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L1195-L1226),
   and salvage if necessary.
   - If call to WiredTiger
     [verify()](https://source.wiredtiger.com/develop/struct_w_t___s_e_s_s_i_o_n.html#a0334da4c85fe8af4197c9a7de27467d3)
     fails, call
     [salvage()](https://source.wiredtiger.com/develop/struct_w_t___s_e_s_s_i_o_n.html#ab3399430e474f7005bd5ea20e6ec7a8e),
     which recovers as much data from a WT data file as possible.
   - If a salvage is unsuccessful, rename the data file with a `.corrupt` suffix.
   - If a data file is missing or a salvage was unsuccessful, [drop the original table from the
     metadata, and create a new, empty
     table](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L1262-L1274)
     under the original name. This allows MongoDB to continue to start up despite present
     corruption.
   - After any salvage operation, [all indexes are
     rebuilt](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/repair_database.cpp#L134-L149)
     for that collection.
5. Validate collection and index consistency
   - [Collection validation](#collection-validation) checks for consistency between the collection
     and indexes. Validate repair mode attempts to fix any inconsistencies it finds.
6. Rebuild indexes
   - If a collection's data has been salvaged or any index inconsistencies are not repairable by
     validate repair mode, [all indexes are
     rebuilt](https://github.com/mongodb/mongo/blob/4406491b2b137984c2583db98068b7d18ea32171/src/mongo/db/repair.cpp#L273-L275).
   - While a unique index is being rebuilt, if any documents are found to have duplicate keys, then
     those documents are inserted into a lost and found collection with the format
     `local.lost_and_found.<collection UUID>`.
7. [Invalidate the replica set
   configuration](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/repair_database_and_check_version.cpp#L460-L485)
   if data has been or could have been modified. This [prevents a repaired node from
   joining](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/repl/replication_coordinator_impl.cpp#L486-L494)
   and threatening the consistency of its replica set.

Additionally:

- When repair starts, it creates a temporary file, `_repair_incomplete` that is only removed when
  repair completes. The server [will not start up
  normally](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/storage_engine_init.cpp#L82-L86)
  as long as this file is present.
- Repair [will restore a
  missing](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/repair_database_and_check_version.cpp#L434)
  `featureCompatibilityVersion` document in the `admin.system.version` to the lower FCV version
  available.

# Startup Recovery

There are three components to startup recovery. The first step, of course, is starting
WiredTiger. WiredTiger will replay its log, if any, from a crash. While the WT log also contains
entries that are specific to WT, most of its entries are to re-insert items into MongoDB's oplog
collection. More detail about WiredTiger's log and its entries are [included in the
appendix](#Cherry-picked-WT-log-Details).

The other two parts of storage startup recovery are for bringing the catalog back into a
consistent state. The catalog typically refers to MongoDB's notion of collections
and indexes, but it's important to note that WT has its own notion of a catalog. The MongoDB
string that identifies a single storage engine table is called an "ident".

The first step of recovering the catalog is to bring MongoDB's catalog in line with
WiredTiger's. This is called reconciliation. Except for rare cases, every MongoDB collection is a
RecordStore and a list of indexes (aka SortedDataInterface). Every record store and index maps to
their own WT table. [The appendix](#Collection-and-Index-to-Table-relationship) describes the
relationship between creating/dropping a collection and the underlying creation/deletion
of a WT table which justifies the following logic. When reconciling, every WT table
that is not "pointed to" by a MongoDB record store or index [gets
dropped](https://github.com/mongodb/mongo/blob/6c9adc9a2d518fa046c7739e043a568f9bee6931/src/mongo/db/storage/storage_engine_impl.cpp#L663-L676 "Github"). A MongoDB record store that points to a WT table that doesn't exist is considered [a
fatal
error](https://github.com/mongodb/mongo/blob/6c9adc9a2d518fa046c7739e043a568f9bee6931/src/mongo/db/storage/storage_engine_impl.cpp#L679-L693 "Github"). An index that doesn't point to a WT table is [ignored and logged](https://github.com/mongodb/mongo/blob/6c9adc9a2d518fa046c7739e043a568f9bee6931/src/mongo/db/storage/storage_engine_impl.cpp#L734-L746 "Github") because there are cetain cases where the catalog entry may reference an index ident which
is no longer present, such as when an unclean shutdown occurs before a checkpoint is taken during
startup recovery.

The second step of recovering the catalog is [reconciling unfinished index builds](https://github.com/mongodb/mongo/blob/6c9adc9a2d518fa046c7739e043a568f9bee6931/src/mongo/db/storage/storage_engine_impl.cpp#L695-L699 "Github"), that could have different outcomes:

- An [index build with a UUID](https://github.com/mongodb/mongo/blob/6c9adc9a2d518fa046c7739e043a568f9bee6931/src/mongo/db/storage/storage_engine_impl.cpp#L748-L751 "Github")
  is an unfinished two-phase build and must be restarted, unless we are
  [resuming it](#resumable-index-builds). This resume information is stored in an internal ident
  written at (clean) shutdown. If we fail to resume the index build, we will clean up the internal
  ident and restart the index build in the background.
- An [unfinished index build on standalone](https://github.com/mongodb/mongo/blob/6c9adc9a2d518fa046c7739e043a568f9bee6931/src/mongo/db/storage/storage_engine_impl.cpp#L792-L794 "Github")
  will be discarded (no oplog entry was ever written saying the index exists).

After storage completes its recovery, control is passed to [replication
recovery](https://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/README.md#startup-recovery "Github"). While storage recovery is responsible for recovering the oplog to meet durability
guarantees and getting the two catalogs in sync, replication recovery takes responsibility for
getting collection data in sync with the oplog. Replication starts replaying oplog from the
`recovery_timestamp + 1`. When WiredTiger takes a checkpoint, it uses the
[`stable_timestamp`](https://github.com/mongodb/mongo/blob/87de9a0cb1/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L2011 "Github") (effectively a `read_timestamp`) for what data should be persisted in the
checkpoint. Every "data write" (collection/index contents, \_mdb_catalog contents) corresponding to an oplog entry with a
timestamp <= the `stable_timestamp` will be included in this checkpoint. None of the data writes
later than the `stable_timestamp` are included in the checkpoint. When the checkpoint is completed, the
`stable_timestamp` is known as the checkpoint's [`checkpoint_timestamp`](https://github.com/mongodb/mongo/blob/834a3c49d9ea9bfe2361650475158fc0dbb374cd/src/third_party/wiredtiger/src/meta/meta_ckpt.c#L921 "Github"). When WiredTiger starts up on a checkpoint,
that checkpoint's timestamp is known as the
[`recovery_timestamp`](https://github.com/mongodb/mongo/blob/87de9a0cb1/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L684 "Github").

## Recovery To A Stable Timestamp

Also known as rollback-to-stable, this is an operation that retains only modifications that are
considered stable. In other words, we are rolling back to the latest checkpoint.

The first step after the node transitions to the rollback state is to stop any operations that can
call into the storage engine. This involves killing all user operations, and stopping and waiting
for active index builds to complete. In addition, we stop internal threads such as the journal
flusher and checkpointer. This is necessary because `WT_CONNECTION::rollback_to_stable` requires all
open cursors to be closed or reset, otherwise `EBUSY` will be returned. In the server we retry on
`EBUSY` until the system quiesces.

Once the system is quiesced, the exclusive global lock is acquired to prevent new operations from
starting. The in-memory representation of the catalog is cleared and the drop pending state is
cleared in the ident reaper as drops may be rolled back. At this point
`WT_CONNECTION::rollback_to_stable` is called. Once we return from this function, the reverse order
of operations is performed. Such as rebuilding the in-memory representation of the catalog, internal
threads are restarted, and two-phase index builds are resumed.

See [here](https://source.wiredtiger.com/develop/arch-rts.html) for WiredTiger's architecture guide
on rollback-to-stable.

See [here](https://github.com/mongodb/mongo/blob/5bd1d0880a7519e54678684b3d243f590936c46a/src/mongo/db/repl/README.md#rollback-recover-to-a-timestamp-rtt)
for more information on what happens in the replication layer during rollback-to-stable.

# File-System Backups

Backups represent a full copy of the data files at a point-in-time. These copies of the data files
can be used to recover data from a consistent state at an earlier time. This technique is commonly
used after a disaster ensued in the database.

The data storage requirements for backups can be large as they correlate to the size of the
database. This can be alleviated by using a technique called incremental backups. Incremental
backups only back up the data that has changed since the last backup.

MongoDB instances used in production should have a strategy in place for capturing and restoring
backups in the case of data loss events.

[Documentation for further reading.](https://docs.mongodb.com/manual/core/backups/)

# Queryable Backup (Read-Only)

This is a feature provided by Ops Manager in which Ops Manager quickly and securely makes a given
snapshot accessible over a MongoDB connection string.

Queryable backups start-up quickly regardless of the snapshot's total data size. They are uniquely
useful for restoring a small subset of data, such as a document that was accidentally deleted or
reading out a single collection. Queryable backups allow access to the snapshot for read-only
operations.

# Checkpoints

Checkpoints provide recovery points that enable the database to load a consistent snapshot of the
data quickly during startup or after a failure. Checkpoints provide basic operation durability in
favor of fast recovery in the event of a crash.

Write-ahead logging, aka [journaling](#journaling), is used in addition to checkpointing to provide
commit-level durability for all operations since the last checkpoint. On startup, all journaled
writes are re-applied to the data from the last checkpoint. Without journaling, all writes between
checkpoints would be lost.

Storage engines need to
[support checkpoints](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/storage_engine.h#L267)
for MongoDB to take advantage of this, otherwise MongoDB will act as an ephemeral data store. The
frequency of these checkpoints is determined by the
['storage.syncPeriodSecs' or 'syncdelay'](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/mongod_options_storage.idl#L86-L93)
options.

The WiredTiger storage engine
[supports checkpoints](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L443-L647)
, which are a read-only, static view of one or more data sources. When WiredTiger takes a
checkpoint, it writes all of the data in a snapshot to the disk in a consistent way across all of
the data files.

To avoid taking unnecessary checkpoints on an idle server, WiredTiger will only take checkpoints for
the following scenarios:

- When the [stable timestamp](../repl/README.md#replication-timestamp-glossary) is greater than or
  equal to the [initial data timestamp](../repl/README.md#replication-timestamp-glossary), we take a
  stable checkpoint, which is a durable view of the data at a particular timestamp. This is for
  steady-state replication.
- The [initial data timestamp](../repl/README.md#replication-timestamp-glossary) is not set, so we
  must take a full checkpoint. This is when there is no consistent view of the data, such as during
  initial sync.

Not only does checkpointing provide us with durability for the database, but it also enables us to
take [backups of the data](#file-system-backups).

# Journaling

MongoDB provides write durability via a persisted change log for replicated writes and persistence
of non-replicated writes. The replicated change log and non-replicated collections in WiredTiger are
journaled, i.e. written out to disk. The user writes themselves, however, on a --replSet server, do
not need to be written out to disk to guarantee persistence on the server.

All replicated server writes have matching change log entries representing the changes done. The
change log is stored in the `local.oplog.rs` namespace, which is set up as a capped collection so
that old unneeded log entries are eventually removed. Replication uses the oplog collection to copy
data from one server to another.

WiredTiger journals any collection or index with `log=(enabled=true)` specified at creation. Such
collection and index tables are specially logged / journaled to disk when requested. The MongoDB
change log stored in the oplog collection is journaled, along with most non-replicated `local`
database collections, when the server instance is started with `--replSet`. In standalone mode,
however, MongoDB does not create the `local.oplog.rs` collection and all collections are journaled.

Durability of journaled collections and indexes is done by periodic or triggered journal flushes
that specifically flush only journaled writes to disk. MongoDB can disable journaling, such as in
standalone mode, so that the periodic journal flushes do not occur. Instead,
[Checkpoints](#checkpoints), which flush all writes to disk regardless of journal settings, are
taken whenever durability of a write is requested and journaling is disabled. Syncing only journaled
collection entries to disk is cheaper than syncing all data writes.

Data durability is essential for recovery after server shutdown. Data must be persisted to disk to
survive process restart, either in the form of the journal or as the write itself. Server startup
recovery will open the storage engine at the last-made data checkpoint, and all of the journaled
writes flushed to disk will also be found even if they occurred after the last checkpoint. The
replication layer decides what to apply of the change log (oplog collection) past the checkpoint for
cross replica set data consistency. For example, the storage engine might recover data up to time
T9, but the journaled oplog recovered could go up to T20, say. It is a replication level decision to
apply (or not apply) T10 through T20.

_Code spelunking starting points:_

- [_The JournalFlusher class_](https://github.com/mongodb/mongo/blob/767494374cf12d76fc74911d1d0fcc2bbce0cd6b/src/mongo/db/storage/control/journal_flusher.h)
  - Perioidically and upon request flushes the journal to disk.
- [_Code that ultimately calls flush journal on WiredTiger_](https://github.com/mongodb/mongo/blob/767494374cf12d76fc74911d1d0fcc2bbce0cd6b/src/mongo/db/storage/wiredtiger/wiredtiger_session_cache.cpp#L241-L362)
  - Skips flushing if ephemeral mode engine; may do a journal flush or take a checkpoint depending
    on server settings.
- [_Control of whether journaling is enabled_](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h#L451)
  - 'durable' confusingly means journaling is enabled.
- [_Whether WT journals a collection_](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_util.cpp#L560-L580)

# Global Lock Admission Control

There are 2 separate ticketing mechanisms placed in front of the global lock acquisition. Both aim to limit the number of concurrent operations from overwhelming the system. Before an operation can acquire the global lock, it must acquire a ticket from one, or both, of the ticketing mechanisms. When both ticket mechanisms are necessary, the acquisition order is as follows:

1. Flow Control - Required only for global lock requests in MODE_IX
2. Execution Control - Required for all global lock requests

Flow Control is in place to prevent a majority of secondaries from falling behind in replication, whereas Execution Control aims to limit the number of concurrent storage engine transactions on a single node.

## Admission Priority

Associated with every operation is an admission priority, stored as a part of the [AdmissionContext](https://github.com/mongodb/mongo/blob/r6.3.0-rc0/src/mongo/util/concurrency/admission_context.h#L40). By default, operations are 'normal' priority.

In the Flow Control ticketing system, operations of 'immediate' priority bypass ticket acquisition regardless of ticket availability. Tickets that are not 'immediate' priority must throttle when there are no tickets available in both Flow Control and Execution Control.

Flow Control is only concerned whether an operation is 'immediate' priority and does not differentiate between 'normal' and 'low' priorities. The current version of Execution Control relies on admission priority to administer tickets when the server is under load.

**AdmissionContext::Priority**

- `kExempt` - Reserved for operations critical to availability (e.g replication workers), or observability (e.g. FTDC), and any operation releasing resources (e.g. committing or aborting prepared transactions).
- `kNormal` - An operation that should be throttled when the server is under load. If an operation is throttled, it will not affect availability or observability. Most operations, both user and internal, should use this priority unless they qualify as 'kLow' or 'kExempt' priority.

[See AdmissionContext::Priority for more details](https://github.com/mongodb/mongo/blob/r7.0.0-rc0/src/mongo/util/concurrency/admission_context.h#L45-L67).

## Execution Control

A ticketing mechanism that limits the number of concurrent storage engine transactions in a single mongod to reduce contention on storage engine resources.

### Ticket Management

There are 2 separate pools of available tickets: one pool for global lock read requests (MODE_S/MODE_IS), and one pool of tickets for global lock write requests (MODE_IX).

As of v7.0, the size of each ticket pool is managed dynamically by the server to maximize throughput. Details of the algorithm can be found [here](https://github.com/mongodb/mongo/blob/master/src/mongo/db/admission/README.md). This dynamic management can be disabled by specifying the size of each pool manually via server parameters `storageEngineConcurrentReadTransactions` (read ticket pool) and `storageEngineConcurrentWriteTransactions` (write ticket pool).

Each pool of tickets is maintained in a [TicketHolder](https://github.com/mongodb/mongo/blob/r6.3.0-rc0/src/mongo/util/concurrency/ticketholder.h#L52). Tickets distributed from a given TicketHolder will always be returned to the same TicketHolder (a write ticket will always be returned to the TicketHolder with the write ticket pool).

## Flow Control

The Flow Control mechanism aims to keep replica set majority committed lag less than or equal to a
configured maximum. The default value for this maximum lag is 10 seconds. The Flow Control mechanism
starts throttling writes on the primary once the majority committed replication lag reaches a
threshold percentage of the configured maximum. The mechanism uses a "ticket admission"-based
approach to throttle writes. With this mechanism, in a given period of 1 second, a fixed number of
"flow control tickets" is available. Operations must acquire a flow control ticket in order to
acquire a global IX lock to execute a write. Acquisition attempts that occur after this fixed number
has been granted will stall until the next 1 second period. Certain system operations circumvent the
ticket admission mechanism and are allowed to proceed even when there are no tickets available.

To address the possibility of this Flow Control mechanism causing indefinite stalls in
Primary-Secondary-Arbiter replica sets in which a majority cannot be established, the mechanism only
executes when read concern majority is enabled. Additionally, the mechanism can be disabled by an
admin.

Flow Control is configurable via several server parameters. Additionally, currentOp, serverStatus,
database profiling, and slow op log lines include Flow Control information.

### Flow Control Ticket Admission Mechanism

The ticket admission Flow Control mechanism allows a specified number of global IX lock acquisitions
every second. Most global IX lock acquisitions (except for those that explicitly circumvent Flow
Control) must first acquire a "Flow Control ticket" before acquiring a ticket for the lock. When
there are no more flow control tickets available in a one second period, remaining attempts to
acquire flow control tickets stall until the next period, when the available flow control tickets
are replenished. It should be noted that there is no "pool" of flow control tickets that threads
give and take from; an independent mechanism refreshes the ticket counts every second.

When the Flow Control mechanism refreshes available tickets, it calculates how many tickets it
should allow in order to address the majority committed lag.

The Flow Control mechanism determines how many flow control tickets to replenish every period based
on:

1. The current majority committed replication lag with respect to the configured target maximum
   replication lag
1. How many operations the secondary sustaining the commit point has applied in the last period
1. How many IX locks per operation were acquired in the last period

### Configurable constants

Criterion #2 determines a "base" number of flow control tickets to be used in the calculation. When
the current majority committed lag is greater than or equal to a certain configurable threshold
percentage of the target maximum, the Flow Control mechanism scales down this "base" number based on
the discrepancy between the two lag values. For some configurable constant 0 < k < 1, it calculates
the following:

`base * k ^ ((lag - threshold)/threshold) * fudge factor`

The fudge factor is also configurable and should be close to 1. Its purpose is to assign slightly
lower than the "base" number of flow control tickets when the current lag is close to the threshold.
Criterion #3 is then multiplied by the result of the above calculation to translate a count of
operations into a count of lock acquisitions.

When the majority committed lag is less than the threshold percentage of the target maximum, the
number of tickets assigned in the previous period is used as the "base" of the calculation. This
number is added to a configurable constant (the ticket "adder" constant), and the sum is multiplied
by another configurable constant (the ticket "multiplier" constant). This product is the new number
of tickets to be assigned in the next period.

When the Flow Control mechanism is disabled, the ticket refresher mechanism always allows one
billion flow control ticket acquisitions per second. The Flow Control mechanism can be disabled via
a server parameter. Additionally, the mechanism is disabled on nodes that cannot accept writes.

Criteria #2 and #3 are determined using a sampling mechanism that periodically stores the necessary
data as primaries process writes. The sampling mechanism executes regardless of whether Flow Control
is enabled.

### Oscillations

There are known scenarios in which the Flow Control mechanism causes write throughput to
oscillate. There is no known work that can be done to eliminate oscillations entirely for this
mechanism without hindering other aspects of the mechanism. Work was done (see SERVER-39867) to
dampen the oscillations at the expense of throughput.

### Throttling internal operations

The Flow Control mechanism throttles all IX lock acquisitions regardless of whether they are from
client or system operations unless they are part of an operation that is explicitly excluded from
Flow Control. Writes that occur as part of replica set elections in particular are excluded. See
SERVER-39868 for more details.

# Fast Truncation on Internal Collections

Logical deletes aren't always performant enough to keep up with inserts. To solve this, several internal collections use `CollectionTruncateMarkers` for fast, unreplicated and untimestamped [truncation](http://source.wiredtiger.com/1.4.2/classwiredtiger_1_1_session.html#a80a9ee8697a61a1ad13d893d67d981bb) of expired data, in lieu of logical document deletions.

## CollectionTruncateMarkers

CollectionTruncateMarkers are an in-memory tracking mechanism to support ranged truncates on a collection.

A collection is broken up into a number of truncate markers. Each truncate marker tracks a range in the collection. Newer entries not captured by a truncate marker are tracked by an in-progress "partial marker".

```
                                CollectionTruncateMarkers
               _______________________________________
              |             |  ......   |             |   Partial Marker
              |_____________|___________|_____________|__________
               Oldest Marker             Newest Marker
Min RecordId <------------------------------------------------<--- Max RecordId


                    Truncate Marker
         _______________________________________
        | . Last Record's RecordId              |
        | . Last Record's Wall Time             |
        | . Bytes in Marker                     |
        | . Number of records in Marker         |
        |_______________________________________|
                                               ^
                                               |
                                           Last Record
                              Marks the end of the marker's range
                        Most recent record at the time of marker creation
```

A new truncate marker is created when either:

1. An insert causes the in-progress "partial marker" segment to contain more than the minimum bytes needed for a truncate marker.
   - The record inserted serves as the 'last record' of the newly created marker.
2. Partial marker expiration is supported, and an explicit call is made to transform the "partial marker" into a complete truncate marker.
   - Partial marker expiration is supported for change stream collections and ensures that expired documents in a partial marker will eventually be truncated - even if writes to the namespace cease and the partial marker never meets the minimum bytes requirement.

### Requirements & Properties

CollectionTruncateMarkers support collections that meet the following requirements:

- Insert and truncate only. No updates or individual document deletes.
- [Clustered](https://github.com/10gen/mongo/blob/r7.1.0-rc3/src/mongo/db/catalog/README.md#clustered-collections) with no secondary indexes.
- RecordId's in Timestamp order.
- Deletion of content follows RecordId ordering.
  - This is a general property of clustered capped collections.

Collections who use CollectionTruncateMarkers share the following properties:

- Fast counts aren't expected to be accurate.
  - Truncates don't track the count and size of documents truncated in exchange for performance gains.
  - Markers are a best effort way to keep track of the size metrics and when to truncate expired data.
- Collections aren't expected to be consistent between replica set members.
  - Truncates are unreplicated, and nodes may truncate ranges at different times.
- No snapshot read concern support (ex: [SERVER-78296](https://jira.mongodb.org/browse/SERVER-78296)).
  - Deleting with untimestamped, unreplicated range truncation means point-in-time reads may see inconsistent data.

Each collection utilizing CollectionTruncateMarkers must implement its [own policy](https://github.com/mongodb/mongo/blob/r7.1.0-rc3/src/mongo/db/storage/collection_truncate_markers.h#L277) to determine when there are excess markers and it is time for truncation.

### In-Memory Initialization

At or shortly after startup, an initial set of CollectionTruncateMarkers are created for each collection. The collection is either scanned or sampled to generate initial markers. Initial truncate markers are best effort, and may hold incorrect estimates about the number of documents and bytes within each marker. Eventually, once the initial truncate markers expire, per truncate marker metrics will converge closer to the correct values.

### Collections that use CollectionTruncateMarkers

- [The oplog](#oplog-truncation) - `OplogTruncateMarkers`
- [Change stream pre images collections](#pre-images-collection-truncation) - `PreImagesTruncateMarkersPerNsUUID`

### Change Stream Collection Truncation

Change stream collection that uses CollectionTruncateMarkers

- pre-images: `<tenantId>_config.system.preimages` in serverless, `config.system.preimages` in dedicated environments.

The change stream pre-images collections has a periodic remover thread ([ChangeStreamExpiredPreImagesRemover](https://github.com/10gen/mongo/blob/r7.1.0-rc3/src/mongo/db/pipeline/change_stream_expired_pre_image_remover.cpp#L71).
The remover thread:

1. Creates the tenant's initial CollectionTruncateMarkers for the tenant if they do not yet exist
   - Lazy initialization of the initial truncate markers is imperative so writes aren't blocked on startup
2. Iterates through each truncate marker. If a marker is expired, issues a truncate of all records older than the marker's last record, and removes the marker from the set.

#### Cleanup After Unclean Shutdown

After an unclean shutdown, all expired pre-images are truncated at startup. WiredTiger truncate cannot guarantee a consistent view of previously truncated data on unreplicated, untimestamped ranges after a crash. Unlike the oplog, the change stream collections aren't logged, don't persist any special timestamps, and it's possible that previously truncated documents can resurface after shutdown.

#### Pre Images Collection Truncation

Each tenant has 1 pre-images collection. Each pre-images collection contains pre-images across all the tenant's pre-image enabled collections.

A pre-images collection is clustered by [ChangeStreamPreImageId](https://github.com/10gen/mongo/blob/r7.1.0-rc3/src/mongo/db/pipeline/change_stream_preimage.idl#L69), which implicitly orders pre-images first by their `'nsUUID'` (the UUID of the collection the pre-image is from), their `'ts'` (the timestamp associated with the pre-images oplog entry), and then by their `'applyOpsIndex'` (the index into the applyOps oplog entry which generated the pre-image, 0 if the pre-image isn't from an applyOps oplog entry).

There is a set of CollectionTruncateMarkers for each 'nsUUD' within a tenant's pre-images collection, `PreImagesTruncateMarkersPerNsUUID`.

In a serverless environment, each tenant has a set 'expireAfterSeconds' parameter. An entry is expired if the 'wall time' associated with the pre-image is more than 'expireAfterSeconds' older than the node's current wall time.

In a dedicated environment, a pre-image is expired if either (1) 'expireAfterSeconds' is set and the pre-image is expired by it or (2) it's 'ts' is less than or equal to the oldest oplog entry timestamp.

For each tenant, `ChangeStreamExpiredPreImagesRemover` iterates over each set of `PreImagesTruncateMarkersPerNsUUID`, and issues a ranged truncate from the truncate marker's last record to the the minimum RecordId for the nsUUID when there is an expired truncate marker.

### Code spelunking starting points:

- [The CollectionTruncateMarkers class](https://github.com/mongodb/mongo/blob/r7.1.0-rc3/src/mongo/db/storage/collection_truncate_markers.h#L78)
  - The main api for CollectionTruncateMarkers.
- [The OplogTruncateMarkers class](https://github.com/10gen/mongo/blob/r7.1.0-rc3/src/mongo/db/storage/wiredtiger/wiredtiger_record_store_oplog_truncate_markers.h)
  - Oplog specific truncate markers.
- [The PreImagesTruncateMarkersPerNsUUID class](https://github.com/10gen/mongo/blob/r7.1.0-rc3/src/mongo/db/change_stream_pre_images_truncate_markers_per_nsUUID.h#L62)
  - Truncate markers for a given nsUUID captured within a pre-images collection.
- [The PreImagesTruncateManager class](https://github.com/10gen/mongo/blob/r7.1.0-rc3/src/mongo/db/change_stream_pre_images_truncate_manager.h#L70)
  - Manages pre image truncate markers for each tenant.

# Oplog Collection

The `local.oplog.rs` collection maintains a log of all writes done on a server that should be
replicated by other members of its replica set. All replicated writes have corresponding oplog
entries; non-replicated collection writes do not have corresponding oplog entries. On a primary, an
oplog entry is written in the same storage transaction as the write it logs; a secondary writes the
oplog entry and then applies the write reflected therein in separate transactions. The oplog
collection is only created for servers started with the `--replSet` setting. The oplog collection is
a capped collection and therefore self-deleting per the default oplog size. The oplog can be resized
by the user via the `replSetResizeOplog` server command.

A write's persistence is guaranteed when its oplog entry reaches disk. The log is periodically
synced to disk, i.e. [journaled](#journaling). The log can also be immediately synced to disk by an
explicit request to fulfill the durability requirements of a particular write. For example:
replication may need to guarantee a write survives server restart before proceeding, for
correctness; or a user may specify a `j:true` write concern to request the same durability. The data
write itself is not written out to disk until the next periodic [checkpoint](#checkpoints) is taken.
The default log syncing frequency is much higher than the checkpoint default frequency because
syncing the log to disk is cheaper than syncing everything to disk.

The oplog is read by secondaries that then apply the writes therein to themselves. Secondaries can
'fall off the oplog' if replication is too slow and the oplog capped max size is too small: the sync
source may delete oplog entries that a secondary still needs to read. The oplog is also used on
startup recovery to play writes forward from a checkpoint; and it is manipulated -- undone or
reapplied -- for replication rollback.

## Oplog Visibility

MongoDB supports concurrent writes. This means that there are out-of-order commits and 'oplog holes'
can momentarily exist when one write with a later timestamp commits before a concurrent write with
an earlier timestamp. Timestamps are assigned prior to storage transaction commit. Out-of-order
writes are supported because otherwise writes must be serialized, which would harm performance.

Oplog holes must be tracked so that oplog read cursors do not miss data when reading in timestamp
order. Unlike typical collections, the key for a document in the oplog is the timestamp itself.
Because collection cursors return data in key order, cursors on the oplog will return documents in
timestamp order. Oplog readers therefore fetch a timestamp guaranteed not to have holes behind it
and use that timestamp to open a storage engine transaction that does not return entries with later
timestamps. The following is a demonstrative example of what this oplog visibility rule prevents:

Suppose there are two concurrent writers **A** and **B**. **Writer A** opens a storage transaction
first and is assigned a commit timestamp of **T5**; then **Writer** **B** opens a transaction and
acquires a commit timestamp **T6**. The writers are using different threads so **Writer B** happens
to commit first. The oplog now has a 'hole' for timestamp **T5**. A reader opening a read
transaction at this time could now see up to the **T6** write but miss the **T5** write that has not
committed yet: the cursor would see T1, T2, T3, T4, T6. This would be a serious replica set data
consistency problem if secondary replica set members querying the oplog of their sync source could
unknowingly read past these holes and miss the data therein.

| Op       | Action             | Result                                       |
| -------- | ------------------ | -------------------------------------------- |
| Writer A | open transaction   | assigned commit timestamp T5                 |
| Writer B | open transaction   | assigned commit timestamp T6                 |
| Writer B | commit transation  | T1,T2,T3,T4,T6 are visible to new readers    |
| Reader X | open transaction   | gets a snapshot of T1-T4 and T6              |
| Writer A | commit transaction | T1,T2,T3,T4,T5,T6 are visible to new readers |
| Reader X | close transaction  | returns T1,T2,T3,T4,T6, missing T5           |

The in-memory 'no holes' point of the oplog is tracked in order to avoid data inconsistency across
replica set members. The 'oplogReadTimestamp' tracks the in-memory no holes point and is continually
advanced as new oplog writes occur and holes disappear. Forward cursor oplog readers without a
specified timestamp set at which to read (secondary callers) will automatically use the
`oplogReadTimestamp` to avoid missing entries due to oplog holes. This is essential for secondary
replica set members querying the oplog of their sync source so they do not miss any oplog entries:
subsequent `getMores` will fetch entries as they become visibile without any holes behind them.
Backward cursor oplog readers bypass the oplog visibility rules to see the latest oplog entries,
disregarding any oplog holes.

## Oplog Truncation

The oplog collection can be truncated both at the front end (most recent entries) and the back end (the oldest entries). The capped setting on the oplog collection causes the oldest oplog entries to be deleted when new writes increase the collection size past the cap. MongoDB using the WiredTiger storage engine with `--replSet` handles oplog collection deletion specially via OplogTruncateMarkers, an oplog specific implementation of the [CollectionTruncateMarkers](#collectionTruncateMarkers) mechanism, ignoring the generic capped collection deletion mechanism. The front of the oplog may be truncated back to a particular timestamp during replication startup recovery or replication rollback.

A new truncate marker is created when the in-progress marker segment contains more than the minimum bytes needed to
complete the segment; and the oldest truncate marker's oplog is deleted when the oplog size exceeds
its cap size setting.

### Special Timestamps That Will Not Be Truncated

The WiredTiger integration layer's `OplogTruncateMarkers` implementation will stall deletion waiting for
certain significant tracked timestamps to move forward past entries in the oldest truncate marker. This is
done for correctness. Backup pins truncation in order to maintain a consistent view of the oplog;
and startup recovery after an unclean shutdown and rollback both require oplog history back to
certain timestamps.

### Min Oplog Retention

WiredTiger `OplogTruncateMarkers` obey an `oplogMinRetentionHours` configurable setting. When
`oplogMinRetentionHours` is active, the WT `OplogTruncateMarkers` will only truncate the oplog if a
truncate marker (a sequential range of oplog) is not within the minimum time range required to
remain.

### Oplog Hole Truncation

MongoDB maintains an `oplogTruncateAfterPoint` timestamp while in `PRIMARY` and `SECONDARY`
replication modes to track persisted oplog holes. Replication startup recovery uses the
`oplogTruncateAfterPoint` timestamp, if one is found to be set, to truncate all oplog entries after
that point. On clean shutdown, there are no oplog writes and the `oplogTruncateAfterPoint` is
cleared. On unclean shutdown, however, parallel writes can be active and therefore oplog holes can
exist. MongoDB allows secondaries to read their sync source's oplog as soon as there are no
_in-memory_ oplog holes, ensuring data consistency on the secondaries. Primaries, therefore, can
allow oplog entries to be replicated and then lose that data themselves, in an unclean shutdown,
before the replicated oplog entries become persisted. Primaries use the `oplogTruncateAfterPoint`
to continually track oplog holes on disk in order to eliminate them after an unclean shutdown.
Additionally, secondaries apply batches of oplog entries out of order and similarly must use the
`oplogTruncateAfterPoint` to track batch boundaries in order to avoid unknown oplog holes after an
unclean shutdown.

# Operation Resource Consumption Metrics

MongoDB supports collecting per-operation resource consumption metrics. These metrics reflect the
impact operations have on the server. They may be aggregated per-database and queried by an
aggregation pipeline stage `$operationMetrics`.

Per-operation metrics collection may be enabled with the
`profileOperationResourceConsumptionMetrics` server parameter (default off). When the parameter is
enabled, operations collect resource consumption metrics and report them in the slow query logs. If
profiling is enabled, these metrics are also profiled.

Per-database aggregation may be enabled with the `aggregateOperationResourceConsumptionMetrics`
server parameter (default off). When this parameter is enabled, in addition to the behavior
described by the profiling server parameter, operations will accumulate metrics to global in-memory
per-database counters upon completion. Aggregated metrics may be queried by using the
`$operationMetrics` aggregation pipeline stage. This stage returns an iterable, copied snapshot of
all metrics, where each document reports metrics for a single database.

Metrics are not cleared for dropped databases, which introduces the potential to slowly leak memory
over time. Metrics may be cleared globally by supplying the `clearMetrics: true` flag to the
pipeline stage or restarting the process.

## Limitations

Metrics are not collected for all operations. The following limitations apply:

- Only operations from user connections collect metrics. For example, internal connections from
  other replica set members do not collect metrics.
- Metrics are only collected for a specific set of commands. Those commands override the function
  `Command::collectsResourceConsumptionMetrics()`.
- Metrics for write operations are only collected on primary nodes.
  - This includes TTL index deletions.
- All attempted write operations collect metrics. This includes writes that fail or retry internally
  due to write conflicts.
- Read operations are attributed to the replication state of a node. Read metrics are broken down
  into whether they occurred in the primary or secondary replication states.
- Index builds collect metrics. Because index builds survive replication state transitions, they
  only record aggregated metrics if the node is currently primary when the index build completes.
- Metrics are not collected on `mongos` and are not supported or tested in sharded environments.
- Storage engines other than WiredTiger do not implement metrics collection.
- Metrics are not adjusted after replication rollback.

## Document and Index Entry Units

In addition to reporting the number of bytes read to and written from the storage engine, MongoDB
reports certain derived metrics: units read and units written.

Document units and index entry units are metric calculations that attempt to account for the
overhead of performing storage engine work by overstating operations on smaller documents and index
entries. For each observed datum, a document or index entry, a unit is calculated as the following:

```
units = ceil (datum bytes / unit size in bytes)
```

This has the tendency to overstate small datums when the unit size is large. These unit sizes are
tunable with the server parameters `documentUnitSizeBytes` and `indexEntryUnitSizeBytes`.

## Total Write Units

For writes, the code also calculates a special combined document and index unit. The code attempts
to associate index writes with an associated document write, and takes those bytes collectively to
calculate units. For each set of bytes written, a unit is calculated as the following:

```
units = ceil (set bytes / unit size in bytes)
```

To associate index writes with document writes, the algorithm is the following:
Within a storage transaction, if a document write precedes as-yet-unassigned index writes, assign
such index bytes with the preceding document bytes, up until the next document write.
If a document write follows as-yet-unassigned index writes, assign such index bytes with the
following document bytes.

The `totalUnitWriteSizeBytes` server parameter affects the unit calculation size for the above
calculation.

## CPU Time

Operations that collect metrics will also collect the amount of active CPU time spent on the command
thread. This is reported as `cpuNanos` and is provided by the `OperationCPUTimer`.

The CPU time metric is only supported on certain flavors of Linux. It is implemented using
`clock_gettime` and `CLOCK_THREAD_CPUTIME_ID`, which has limitations on certain systems. See the
[man page for clock_gettime()](https://linux.die.net/man/3/clock_gettime).

## Example output

The $operationMetrics stage behaves like any other pipeline cursor, and will have the following
schema, per returned document:

```
{
  db: "<dbname>",
  // Metrics recorded while the node was PRIMARY. Summed with secondaryMetrics metrics gives total
  // metrics in all replication states.
  primaryMetrics: {
    // The number of document bytes read from the storage engine
    docBytesRead: 0,
    // The number of document units read from the storage engine
    docUnitsRead: 0,
    // The number of index entry bytes read from the storage engine
    idxEntryBytesRead: 0,
    // The number of index entry units read from the storage engine
    idxEntryUnitsRead: 0,
    // The number of document units returned by query operations
    docUnitsReturned: 0,
    // These fields are ALWAYS ZERO and only present for backwards compatibility:
    cursorSeeks: 0,
    keysSorted: 0,
    sorterSpills: 0,
  },
  // Metrics recorded while the node was SECONDARY
  secondaryMetrics: {
    docBytesRead: 0,
    docUnitsRead: 0,
    idxEntryBytesRead: 0,
    idxEntryUnitsRead: 0,
    docUnitsReturned: 0,
    // These fields are ALWAYS ZERO and only present for backwards compatibility:
    cursorSeeks: 0,
    keysSorted: 0,
    sorterSpills: 0,
  },
  // The amount of active CPU time used by all operations
  cpuNanos: 0,
  // The number of document bytes attempted to be written to or deleted from the storage engine
  docBytesWritten: 0,
  // The number of document units attempted to be written to or deleted from the storage engine
  docUnitsWritten: 0,
  // The number of index entry bytes attempted to be written to or deleted from the storage engine
  idxEntryBytesWritten: 0,
  // The number of index entry units attempted to be written to or deleted from the storage engine
  idxEntryUnitsWritten: 0,
  // The total number of document plus associated index entry units attempted to be written to
  // or deleted from the storage engine
  totalUnitsWritten: 0
}
```

# Clustered Collections

Clustered collections store documents ordered by their cluster key on the RecordStore. The cluster
key must currently be `{_id: 1}` and unique.
Clustered collections may be created with the `clusteredIndex` collection creation option. The
`clusteredIndex` option accepts the following formats:

- A document that specifies the clustered index configuration.
  ```
  {clusteredIndex: {key: {_id: 1}, unique: true}}
  ```
- A legacy boolean parameter for backwards compatibility with 5.0 time-series collections.
  ```
  {clusteredIndex: true}
  ```

Like a secondary TTL index, clustered collections can delete old data when created with the
`expireAfterSeconds` collection creation option.

Unlike regular collections, clustered collections do not require a separate index from cluster key
values to `RecordId`s, so they lack an index on \_id. While a regular collection must access two
different tables to read or write to a document, a clustered collection requires a single table
access. Queries over the \_id key use bounded collection scans when no other index is available.

## Time Series Collections

A time-series collection is a view of an internal clustered collection named
`system.buckets.<name>`, where `<name>` is the name of the time-series collection. The cluster key
values are ObjectId's.

For more information on time-series collections, see the [timeseries/README][].

[timeseries/README]: ../timeseries/README.md

## Capped clustered collections

Capped clustered collections are available internally. Unlike regular capped collections, clustered
capped collections require TTL-based deletion in lieu of size-based deletion. Because on clustered
collections the natural order is the cluster key order rather than the insertion order, capped
deletions remove the documents with lowest cluster key value, which may not be the oldest documents
inserted. In order to guarantee capped insert-order semantics the caller should insert monotonically
increasing cluster key values.

Because unlike regular capped collections, clustered collections do not need to preserve insertion
order, they allow non-serialized concurrent writes. In order to avoid missing documents while
tailing a clustered collection, the user is required to enforce visibility rules similar to the ['no
holes' point](../storage/README.md#oplog-visibility). Majority read concern is similarly required.

## Clustered RecordIds

`RecordId`s can be arbitrarily long and are encoded as binary strings using each document's cluster
key value, rather than a 64-bit integer. The binary string is generate using `KeyString` and
discards the `TypeBits`. The [`kSmallStr` format](https://github.com/mongodb/mongo/blob/r5.2.0/src/mongo/db/record_id.h#L330)
supports small binary strings like the ones used for time series buckets. The [`kLargeStr` format](https://github.com/mongodb/mongo/blob/r5.2.0/src/mongo/db/record_id.h#L336)
supports larger strings and can be cheaply passed by value.

Secondary indexes append `RecordId`s to the end of the `KeyString`. Because a `RecordId` in a
clustered collection can be arbitrarily long, its size is appended at the end and [encoded](https://github.com/mongodb/mongo/blob/r5.2.0/src/mongo/db/storage/key_string.cpp#L608)
right-to-left over up to 4 bytes, using the lower 7 bits of a byte, the high bit serving as a
continuation bit.

## Legacy Catalog Formats That Still Require Support

### Legacy Indexes

We perform stricter checks in index key pattern validation to v:2 indexes, limiting indexes created
in MongoDB 3.4+ to the following:

- numbers > 0 (ascending)
- numbers < 0 (descending)
- strings (special index types)

However, legacy indexes (indexes that were created pre-3.4) still need to be handled in the case
where a customer with legacy indexes upgrades to MongoDB 3.4+. The server treats any non-negative
numerical index key and non-numerical index key value as an ascending index, and treats negative
numerical values as descending. The exception to this is any form of a negative 0 (-0, -0.0, etc);
these are treated as ascending. Details on how these values are treated can be found in [ordering.h](https://github.com/10gen/mongo/blob/master/src/mongo/bson/ordering.h), and examples of legacy indexes can be found in [this test](https://github.com/10gen/mongo/blob/master/src/mongo/bson/ordering_test.cpp).

# Glossary

**DDL**: Acronym for Data Description Language or Data Definition Language used generally in the
context of relational databases. DDL operations in the MongoDB context include index and collection
creation or drop, as well as `collMod` operations.

**snapshot**: A snapshot consists of a consistent view of data in the database. When a snapshot is
opened with a timestamp, snapshot only shows data committed with a timestamp less than or equal
to the snapshot's timestamp.

**storage transaction**: A concept provided by a pluggable storage engine through which changes to
data in the database can be performed. In order to satisfy the MongoDB pluggable storage engine
requirements for atomicity, consistency, isolation, and durability, storage engines typically use
some form of transaction. In contrast, a multi-document transaction in MongoDB is a user-facing
feature providing similar guarantees across many nodes in a sharded cluster; a storage transaction
only provides guarantees within one node.

[`BSONObj::woCompare`]: https://github.com/mongodb/mongo/blob/v4.4/src/mongo/bson/bsonobj.h#L460
[`BSONElement::compareElements`]: https://github.com/mongodb/mongo/blob/v4.4/src/mongo/bson/bsonelement.cpp#L285
[`Ordering`]: https://github.com/mongodb/mongo/blob/v4.4/src/mongo/bson/ordering.h
[initial sync]: ../repl/README.md#initial-sync
