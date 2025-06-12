# Catalog Internals

The catalog is where MongoDB stores information about the collections and indexes for a replica set
node. In some contexts we refer to this as metadata and to operations changing this metadata as
[DDL](#glossary) (Data Definition Language) operations. The catalog is persisted as a table with
BSON documents that each describe properties of a collection and its indexes. An in-memory catalog
caches this data for more efficient access.

The catalog provides a mapping from logical user-level namespaces to durable storage engine entities and provides a concurrency control layer to safely modify collections and indexes metadata for DDL operations.

See the [Storage Engine API](../storage/README.md) for relevant information.

## Durable Catalog

Catalog information is persisted as storage table with the `_mdb_catalog` [ident](../storage/README.md#idents). Each entry in this table is indexed with a 64-bit `RecordId`, referred to as the `Catalog ID`, and contains a BSON document that describes the properties of a collection and its indexes.

To manage the `_mdb_catalog` data efficiently and maintain correct layering between components, the server divides responsibilities between two key modules:

- `MDBCatalog` class

  - Persistence Layer: Manages the physical storage of the `_mdb_catalog` table.
  - BSON Entries: Reads and writes entries as raw BSON documents.
  - Field Awareness: Knows only about top-level BSON fields such as `ident`, `idxIdent`, `md`, and `ns`. It does not interpret or validate higher-level semantics of an `_mdb_catalog` entry.

- `durable_catalog` namespace

  - Parsing & Serialization: Knows how to interpret and generate the BSON documents for the `_mdb_catalog` entries, translating between server abstractions (like `CollectionOptions`) and the on-disk format.
  - Abstraction Layer: Serves as the main entry point for server components to interact with the `_mdb_catalog`, abstracting away BSON parsing and storage details.

**Example Flow**: Creating a Collection

- The user issues `db.createCollection(<collName>, <options>)`.
- The 'options' are represented internally as ['CollectionOptions'](https://github.com/mongodb/mongo/blob/88c79d8e8221cb2a73cc324b0bc21e7f3211fa63/src/mongo/db/catalog/collection_options.h).
- The `durable_catalog` translates information about the new collection (such as `CollectionOptions` and other details from the command path) into a properly formatted BSON document suitable for storage in the `_mdb_catalog`. It dispatches the document and `RecordStore::Options` for the new collection to the `MDBCatalog`.
- The `MDBCatalog` writes the document directly to the `_mdb_catalog`. It then uses the `RecordStore::Options` to create a `RecordStore` which allocates space for the new collection on disk.

Starting in v5.2, catalog entries for time-series collections have a new flag called
`timeseriesBucketsMayHaveMixedSchemaData` in the `md` field. Time-series collections upgraded from
versions earlier than v5.2 may have mixed-schema data in buckets. This flag gets set to `true` as
part of the upgrade process and is removed as part of the downgrade process through the
[collMod command](https://github.com/mongodb/mongo/blob/cf80c11bc5308d9b889ed61c1a3eeb821839df56/src/mongo/db/catalog/coll_mod.cpp#L644-L663).

Starting in v7.1, catalog entries for time-series collections have a new flag called
`timeseriesBucketingParametersHaveChanged` in the `md` field.

**Example**: an entry in the mdb catalog for a collection `test.employees` with an in-progress
index build on `{lastName: 1}`:

```
 {'ident': 'collection-8a3a1418-4f05-44d6-aca7-59a2f7b30248',
  'idxIdent': {'_id_': 'index-831b659d-59bd-42b8-b541-2fa42cbabfcb',
               'lastName_1': 'index-b5640da1-34c2-417f-9471-825f764b7bc1'},
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
            "_id_" : "index-41d46f11-b871-466a-9442-af463940ae8e"
        },
        "ns" : "test.system.views",
        "ident" : "collection-8a3a1418-4f05-44d6-aca7-59a2f7b30248"
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
            "_id_" : "index-43bc2c01-b105-4442-82ef-d97f8a4de095",
            "a_1" : "index-a22eca47-c9e1-4df4-a043-d10e4cd45b40"
        },
        "ns" : "list_catalog.simple",
        "ident" : "collection-d3575067-0cd9-4239-a9e8-f6af884fc6fe"
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
            "_id_" : "index-96bb151f-3f1b-496c-bd03-9dd83aad2704"
        },
        "ns" : "list_catalog.system.views",
        "ident" : "collection-fe44e5fc-262c-4ef9-b023-5524ac743962"
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
This has implications for applications that read the mdb catalog using $listCatalog rather than the recommended listIndexes
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
            "_id_" : "index-d07644c2-5242-4c91-b006-5714ba378bd0"
        },
        "ns" : "list_catalog.coll",
        "ident" : "collection-1e587b45-8c55-46a3-a74f-c7bc8a8bf937"
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
            "_id_" : "index-b7c92cfe-24bd-4be5-a9c7-aa71fc6bda31"
        },
        "ns" : "list_catalog.coll",
        "ident" : "collection-80216433-9978-43c6-8dbf-d925345c2c13"
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

- catalog ID - to look up or change information from the '\_mdb_catalog' (either through the
  'MDBCatalog' directly, or through the 'durable_catalog' namespace when parsing is helpful).
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

#### ConsistentCollection and stale reference protection for instantiated Collections

To avoid potential stale reference errors by holding `Collection` instances past their lifetime
calls to `CollectionCatalog::establishConsistentCollection` will result in the creation of a
`ConsistentCollection`. This special object should be understood as a reference-counted `Collection`
pointer to the instance used by the catalog that protects against staleness.

This is only an issue with instances created and stored in the `OpenedCollections` object since
abandoning the snapshot will destroy the container and all instances in it. However, this is
transparent to the user so we have to be pessimistic and treat equally all the collections acquired
this way.

The ConsistentCollection instance ensures that whenever we abandon the snapshot there are no users
of the returned `Collection` pointer left. Doing so is a programmer failure and will invariant and
crash the server to avoid dereferencing invalid memory. It is the programmer's responsibility to
address the issue in their code so that they do not hold stale references.

These checks are not present in release builds since they would otherwise introduce unnecessary
overhead across all operations. Only debug builds are susceptible to these checks.

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
these are treated as ascending. Details on how these values are treated can be found in [ordering.h](https://github.com/mongodb/mongo/blob/master/src/mongo/bson/ordering.h), and examples of legacy indexes can be found in [this test](https://github.com/mongodb/mongo/blob/master/src/mongo/bson/ordering_test.cpp).

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
