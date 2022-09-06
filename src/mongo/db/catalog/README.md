# Execution Internals
The storage execution layer provides an interface for higher level MongoDB components, including
query, replication and sharding, to all storage engines compatible with MongoDB. It maintains a
catalog, in-memory and on-disk, of collections and indexes. It also implements an additional (to
whatever a storage engine implements) concurrency control layer to safely modify the catalog while
sustaining correct and consistent collection and index data formatting.

Execution facilitates reads and writes to the storage engine with various persistence guarantees,
builds indexes, supports replication rollback, manages oplog visibility, repairs data corruption
and inconsistencies, and much more.

The main code highlights are: the storage integration layer found in the [**storage/**][] directory;
the lock manager and lock helpers found in the [**concurrency/**][] directory; the catalog found in
the [**catalog/**][] directory; the index build code found in many directories; the various types of
index implementations found in the [**index/**][] directory; the sorter found in the
[**sorter/**][] directory; and the time-series bucket catalog found in the [**timeseries/**][]
directory.

[**storage/**]: https://github.com/mongodb/mongo/tree/master/src/mongo/db/storage
[**concurrency/**]: https://github.com/mongodb/mongo/tree/master/src/mongo/db/concurrency
[**catalog/**]: https://github.com/mongodb/mongo/tree/master/src/mongo/db/catalog
[**index/**]: https://github.com/mongodb/mongo/tree/master/src/mongo/db/index
[**sorter/**]: https://github.com/mongodb/mongo/tree/master/src/mongo/db/sorter
[**timeseries/**]: https://github.com/mongodb/mongo/tree/master/src/mongo/db/timeseries

For more information on the Storage Engine API, see the [storage/README][].

For more information on time-series collections, see the [timeseries/README][].

[storage/README]: https://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/README.md
[timeseries/README]: https://github.com/mongodb/mongo/blob/master/src/mongo/db/timeseries/README.md

# The Catalog

The catalog is where MongoDB stores information about the collections and indexes for a MongoDB
node. In some contexts we refer to this as metadata and to operations changing this metadata as
[DDL](#glossary) (Data Definition Language) operations. The catalog is persisted as a table with
BSON documents that each describe properties of a collection and its indexes. An in-memory catalog
caches the most recent catalog information for more efficient access.

## Durable Catalog

The durable catalog is persisted as a table with the `_mdb_catalog` [ident](#glossary). Each entry
in this table is indexed with a 64-bit `RecordId`, referred to as the catalog ID, and contains a
BSON document that describes the properties of a collection and its indexes. The `DurableCatalog`
class allows read and write access to the durable data.

Starting in v5.2, catalog entries for time-series collections have a new flag called
`timeseriesBucketsMayHaveMixedSchemaData` in the `md` field. Time-series collections upgraded from
versions earlier than v5.2 may have mixed-schema data in buckets. This flag gets set to `true` as
part of the upgrade process and is removed as part of the downgrade process through the
[collMod command](https://github.com/mongodb/mongo/blob/cf80c11bc5308d9b889ed61c1a3eeb821839df56/src/mongo/db/catalog/coll_mod.cpp#L644-L663).

**Example**: an entry in the durable catalog for a collection `test.employees` with an in-progress
index build on `{lastName: 1}`:

```
 {'ident': 'collection-0--2147780727179663754',
  'idxIdent': {'_id_': 'index-1--2147780727179663754',
               'lastName_1': 'index-2--2147780727179663754'},
  'md': {'indexes': [{'backgroundSecondary': False,
                      'multikey': False,
                      'multikeyPaths': {'_id': Binary('\x00', 0)},
                      'ready': True,
                      'spec': {'key': {'_id': 1},
                               'name': '_id_',
                               'v': 2}},
                     {'backgroundSecondary': False,
                      'multikey': False,
                      'multikeyPaths': {'_id': Binary('\x00', 0)},
                      'ready': False,
                      'buildUUID': UUID('d86e8657-1060-4efd-b891-0034d28c3078'),
                      'spec': {'key': {'lastName': 1},
                               'name': 'lastName_1',
                               'v': 2}}],
          'ns': 'test.employees',
          'options': {'uuid': UUID('795453e9-867b-4804-a432-43637f500cf7')}},
  'ns': 'test.employees'}
```

## Collection Catalog
The `CollectionCatalog` class holds in-memory state about all collections in all databases and is a
cache of the [durable catalog](#durable-catalog) state. It provides the following functionality:
 * Register new `Collection` objects, taking ownership of them.
 * Lookup `Collection` objects by their `UUID` or `NamespaceString`.
 * Iterate over `Collection` objects in a database in `UUID` order.
 * Deregister individual dropped `Collection` objects, releasing ownership.
 * Allow closing/reopening the catalog while still providing limited `UUID` to `NamespaceString`
   lookup to support rollback to a point in time.

### Synchronization
Catalog access is synchronized using [read-copy-update][] where reads operate on an immutable
instance and writes on a new instance with its contents copied from the previous immutable instance
used for reads. Readers holding on to a catalog instance will thus not observe any writes that
happen after requesting an instance. If it is desired to observe writes while holding a catalog
instance then the reader must refresh it.

Catalog writes are handled with the `CollectionCatalog::write(callback)` interface. It provides the
necessary [read-copy-update][] abstractions. A writable catalog instance is created by making a
shallow copy of the existing catalog. The actual write is implemented in the supplied callback which
is allowed to throw. Execution of the write callbacks are serialized and may run on a different
thread than the thread calling `CollectionCatalog::write`.

To avoid a bottleneck in the case the catalog contains a large number of collections (being slow to
copy), concurrent writes are batched together. Any thread that enters `CollectionCatalog::write`
while a catalog instance is being copied is enqueued. When the copy finishes, all enqueued write
jobs are run on that catalog instance by the copying thread.

### Collection objects
Objects of the `Collection` class provide access to a collection's properties between
[DDL](#glossary) operations that modify these properties. Modifications are synchronized using
[read-copy-update][]. Reads access immutable `Collection` instances. Writes, such as rename
collection, apply changes to a clone of the latest `Collection` instance and then atomically install
the new `Collection` instance in the catalog. It is possible for operations that read at different
points in time to use different `Collection` objects.

Notable properties of `Collection` objects are:
 * catalog ID - to look up or change information from the DurableCatalog.
 * UUID - Identifier that remains for the lifetime of the underlying MongoDb collection, even across
   DDL operations such as renames, and is consistent between different nodes and shards in a
   cluster.
 * NamespaceString - The current name associated with the collection.
 * Collation and validation properties.
 * Decorations that are either `Collection` instance specific or shared between all `Collection`
   objects over the lifetime of the collection.
 * minimum visible snapshot - The minimum point-in-time snapshot at which the information the
   `Collection` object is valid.

In addition `Collection` objects have shared ownership of:
 * An [`IndexCatalog`](#index-catalog) - an in-memory structure representing the `md.indexes` data
   from the durable catalog.
 * A `RecordStore` - an interface to access and manipulate the documents in the collection as stored
   by the storage engine.

A writable `Collection` may only be requested in an active [WriteUnitOfWork](#WriteUnitOfWork). The
new `Collection` instance is installed in the catalog when the storage transaction commits, but only
after all other `onCommit` [Changes](#Changes) have run. This ensures `onCommit` operations can
write to the writable `Collection` before it becomes visible to readers in the catalog. If the
storage transaction rolls back then the writable `Collection` object is simply discarded and no
change is ever made to the catalog.

A writable `Collection` is a clone of the existing `Collection`, members are either deep or
shallowed copied. Notably, a shallow copy is made for the [`IndexCatalog`](#index-catalog).

The oplog `Collection` follows special rules, it does not use [read-copy-update][] or any other form
of synchronization. Modifications operate directly on the instance installed in the catalog. It is
not allowed to read concurrently with writes on the oplog `Collection`.

Finally, there are two kinds of decorations on `Collection` objects. The `Collection` object derives
from `DecorableCopyable` and requires `Decoration`s to implement a copy-constructor. Collection
`Decoration`s are copied with the `Collection` when DDL operations occur. This is used for to keep
versioned query information per Collection instance. Additionally, there are
`SharedCollectionDecorations` for storing index usage statistics and query settings that are shared
between `Collection` instances across DDL operations.

### Index Catalog
Each `Collection` object owns an `IndexCatalog` object, which in turn has shared ownership of
`IndexCatalogEntry` objects that each again own an `IndexDescriptor` containing an in-memory
presentation of the data stored in the [durable catalog](#durable-catalog).

## Catalog Changes, versioning and the Minimum Visible Snapshot
Every catalog change has a corresponding write with a commit time. When registered `OpObserver`
objects observe catalog changes, they set the minimum visible snapshot of the `Collection` or
`IndexCatalogEntry` object to the commit timestamp. Readers use this timestamp to determine whether
the information cached in the `Collection` and `IndexCatalog` is valid for the point in time at
which they read.

Operations that use collection locks (in any [lockmode](#lock-modes)) can rely on the catalog
information of the collection not changing. However, when unlocking and then relocking, not only
should operations recheck catalog information to ensure it is still valid, they should also make
sure to abandon the storage snapshot, so it is consistent with the in memory catalog.

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
and the oldest_timestamp. Requiring the drop timestamp to reach the checkpointed time ensures that
startup recovery and rollback via recovery to a stable timestamp, which both recover to the last
checkpoint, will never be missing collection or index data that should still exist at the checkpoint
time that is less than the drop timestamp. Requiring the drop timestamp to pass (become older) than
the oldest_timestamp ensures that all reads, which are supported back to the oldest_timestamp,
successfully find the collection or index data.

_Code spelunking starting points:_

* [_The KVDropPendingIdentReaper
  class_](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/kv/kv_drop_pending_ident_reaper.h)
  * Handles the second phase of collection/index drop. Runs when notified.
* [_The TimestampMonitor and TimestampListener
  classes_](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/storage_engine_impl.h#L178-L313)
  * The TimestampMonitor starts a periodic job to notify the reaper of the latest timestamp that is
    okay to reap.
* [_Code that signals the reaper with a
  timestamp_](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/storage_engine_impl.cpp#L932-L949)

# Storage Transactions

Through the pluggable [storage engine API](https://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/README.md), MongoDB executes reads and writes on its storage engine
with [snapshot isolation](#glossary).  The structure used to achieve this is the [RecoveryUnit
class](../storage/recovery_unit.h).

## RecoveryUnit

Each pluggable storage engine for MongoDB must implement `RecoveryUnit` as one of the base classes
for the storage engine API.  Typically, storage engines satisfy the `RecoveryUnit` requirements with
some form of [snapshot isolation](#glossary) with [transactions](#glossary). Such transactions are
called storage transactions elsewhere in this document, to differentiate them from the higher-level
_multi-document transactions_ accessible to users of MongoDB.  The RecoveryUnit controls what
[snapshot](#glossary) a storage engine transaction uses for its reads.  In MongoDB, a snapshot is defined by a
_timestamp_. A snapshot consists of all data committed with a timestamp less than or equal to the
snapshot's timestamp.  No uncommitted data is visible in a snapshot, and data changes in storage
transactions that commit after a snapshot is created, regardless of their timestamps, are also not
visible.  Generally, one uses a `RecoveryUnit` to perform transactional reads and writes by first
configuring the `RecoveryUnit` with the desired
[ReadSource](https://github.com/mongodb/mongo/blob/b2c1fa4f121fdb6cdffa924b802271d68c3367a3/src/mongo/db/storage/recovery_unit.h#L391-L421)
and then performing the reads and writes using operations on `RecordStore` or `SortedDataInterface`,
and finally calling `commit()` on the `WriteUnitOfWork` (if performing writes).

## WriteUnitOfWork

A `WriteUnitOfWork` is the mechanism to control how writes are transactionally performed on the
storage engine.  All the writes (and reads) performed within its scope are part of the same storage
transaction.  After all writes have been staged, one must call `commit()` in order to atomically
commit the transaction to the storage engine.  It is illegal to perform writes outside the scope of
a WriteUnitOfWork since there would be no way to commit them.  If the `WriteUnitOfWork` falls out of
scope before `commit()` is called, the storage transaction is rolled back and all the staged writes
are lost.  Reads can be performed outside of a `WriteUnitOfWork` block; storage transactions outside
of a `WriteUnitOfWork` are always rolled back, since there are no writes to commit.

## Lazy initialization of storage transactions

Note that storage transactions on WiredTiger are not started at the beginning of a `WriteUnitOfWork`
block.  Instead, the transaction is started implicitly with the first read or write operation.  To
explicitly start a transaction, one can use `RecoveryUnit::preallocateSnapshot()`.

## Changes

One can register a `Change` on a `RecoveryUnit` while in a `WriteUnitOfWork`.  This allows extra
actions to be performed based on whether a `WriteUnitOfWork` commits or rolls back.  These actions
will typically update in-memory state to match what was written in the storage transaction, in a
transactional way.  Note that `Change`s are not executed until the destruction of the
`WriteUnitOfWork`, which can be long after the storage engine committed.  Two-phase locking ensures
that all locks are held while a Change's `commit()` or `rollback()` function runs.


# Read Operations

External reads via the find, count, distint, aggregation and mapReduce cmds do not take collection
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
[WiredTigerCursor](https://github.com/mongodb/mongo/blob/r4.4.0-rc13/src/mongo/db/storage/wiredtiger/wiredtiger_cursor.cpp#L48),
[WiredTigerRecoveryUnit::getSession()](https://github.com/mongodb/mongo/blob/r4.4.0-rc13/src/mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.cpp#L303-L305),
[~GlobalLock](https://github.com/mongodb/mongo/blob/r4.4.0-rc13/src/mongo/db/concurrency/d_concurrency.h#L228-L239),
[PlanYieldPolicy::_yieldAllLocks()](https://github.com/mongodb/mongo/blob/r4.4.0-rc13/src/mongo/db/query/plan_yield_policy.cpp#L182),
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

* [_AutoGetCollectionForReadLockFree preserves an immutable CollectionCatalog_](https://github.com/mongodb/mongo/blob/dcf844f384803441b5393664e500008fc6902346/src/mongo/db/db_raii.cpp#L141)
* [_AutoGetCollectionForReadLockFree returns early if already running lock-free_](https://github.com/mongodb/mongo/blob/dcf844f384803441b5393664e500008fc6902346/src/mongo/db/db_raii.cpp#L108-L112)
* [_The lock-free operation flag on the OperationContext_](https://github.com/mongodb/mongo/blob/dcf844f384803441b5393664e500008fc6902346/src/mongo/db/operation_context.h#L298-L300)

## Secondary Reads

The oplog applier applies entries out-of-order to provide parallelism for data replication. This
exposes readers with no set read timestamp to the possibility of seeing inconsistent states of data.
To solve this problem, the oplog applier takes the ParallelBatchWriterMode (PBWM) lock in X mode,
and readers using no read timestamp are expected to take the PBWM lock in IS mode to avoid observing
inconsistent data mid-batch.

Reads on secondaries are able to opt-out of taking the PBWM lock and read at replication's
[lastApplied](../repl/README.md#replication-timestamp-glossary) optime instead (see
[SERVER-34192](https://jira.mongodb.org/browse/SERVER-34192)). LastApplied is used because on
secondaries it is only updated after each oplog batch, which is a known consistent state of data.
This allows operations to avoid taking the PBWM lock, and thus not conflict with oplog application.

AGCFR provides the mechanism for secondary reads. This is implemented by [opting-out of the
ParallelBatchWriterMode
lock](https://github.com/mongodb/mongo/blob/58283ca178782c4d1c4a4d2acd4313f6f6f86fd5/src/mongo/db/db_raii.cpp#L98)
and switching the ReadSource of [eligible
readers](https://github.com/mongodb/mongo/blob/58283ca178782c4d1c4a4d2acd4313f6f6f86fd5/src/mongo/db/storage/snapshot_helper.cpp#L106)
to read at
[kLastApplied](https://github.com/mongodb/mongo/blob/58283ca178782c4d1c4a4d2acd4313f6f6f86fd5/src/mongo/db/storage/recovery_unit.h#L411).

# Write Operations

Operations that write to collections and indexes must take collection locks. Storage engines require
all operations to hold at least a collection IX lock to provide document-level concurrency.
Operations must perform writes in the scope of a WriteUnitOfWork.

## WriteUnitOfWork

All reads and writes in the scope of a WriteUnitOfWork (WUOW) operate on the same storage engine
snapshot, and all writes in the scope of a WUOW are transactional; they are either all committed or
all rolled-back. The WUOW commits writes that took place in its scope by a call to commit(). It
rolls-back writes when it goes out of scope and its destructor is called before a call to commit().

The WriteUnitOfWork has a [`groupOplogEntries` option](https://github.com/mongodb/mongo/blob/fa32d665bd63de7a9d246fa99df5e30840a931de/src/mongo/db/storage/write_unit_of_work.h#L67)
to replicate multiple writes transactionally. This option uses the [`BatchedWriteContext` class](https://github.com/mongodb/mongo/blob/9ab71f9b2fac1e384529fafaf2a819ce61834228/src/mongo/db/batched_write_context.h#L46)
to stage writes and to generate a single applyOps entry at commit, similar to what multi-document
transactions do via the [`TransactionParticipant` class](https://github.com/mongodb/mongo/blob/9ab71f9b2fac1e384529fafaf2a819ce61834228/src/mongo/db/transaction_participant.h#L82).
Unlike a multi-document transaction, the applyOps entry lacks the `lsId` and the `txnNumber`
fields. Callers must ensure that the WriteUnitOfWork does not generate more than 16MB of oplog,
otherwise the operation will fail with `TransactionTooLarge` code.

As of MongoDB 6.0, the `groupOplogEntries` mode is only used by the [BatchedDeleteStage](https://github.com/mongodb/mongo/blob/9676cf4ad8d537518eb1b570fc79bad4f31d8a79/src/mongo/db/exec/batched_delete_stage.h)
for efficient mass-deletes.

See
[WriteUnitOfWork](https://github.com/mongodb/mongo/blob/fa32d665bd63de7a9d246fa99df5e30840a931de/src/mongo/db/storage/write_unit_of_work.h).

## WriteConflictException

Writers may conflict with each other when more than one operation stages an uncommitted write to the
same document concurrently. To force one or more of the writers to retry, the storage engine may
throw a WriteConflictException at any point, up to and including the the call to commit(). This is
referred to as optimistic concurrency control because it allows uncontended writes to commit
quickly. Because of this behavior, most WUOWs are enclosed in a writeConflictRetry loop that retries
the write transaction until it succeeds, accompanied by a bounded exponential back-off.

See [writeConflictRetry](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/concurrency/write_conflict_exception.h).

## TemporarilyUnavailableException

When the server parameter `enableTemporarilyUnavailableExceptions` is enabled (on by default), a
TemporarilyUnavailableException may be thrown inside the server to indicate that an operation cannot
complete without blocking and must be retried. The storage engine may throw a
TemporarilyUnavailableException (converted to a TemporarilyUnavailable error for users) when an
operation is excessively rolled-back in the storage engine due to cache pressure or any reason that
would prevent the operation from completing without impacting concurrent operations. The operation
may be at fault for writing too much uncommitted data, or it may be a victim. That information is
not exposed. However, if this error is returned, it is likely that the operation was the cause of
the problem, rather than a victim.

Before 6.0, this type of error was returned as a WriteConflict and retried indefinitely inside a
writeConflictRetry loop. As of 6.0, MongoDB will retry the operation internally at most
`temporarilyUnavailableMaxRetries` times, backing off for `temporarilyUnavailableBackoffBaseMs`
milliseconds, with a linearly-increasing backoff on each attempt. After this point, the error will
escape the handler and be returned to the client.

If an operation receives a TemporarilyUnavailable error internally, a `temporarilyUnavailableErrors`
counter will be displayed in the slow query logs and in FTDC.

Notably, this behavior does not apply to multi-document transactions, which continue to return a
WriteConflict to the client in this scenario without retrying internally.

See
[wtRcToStatus](https://github.com/mongodb/mongo/blob/c799851554dc01493d35b43701416e9c78b3665c/src/mongo/db/storage/wiredtiger/wiredtiger_util.cpp#L178-L183)
where we throw the exception in WiredTiger.
See [TemporarilyUnavailableException](https://github.com/mongodb/mongo/blob/c799851554dc01493d35b43701416e9c78b3665c/src/mongo/db/concurrency/temporarily_unavailable_exception.h#L39-L45).
## Collection and Index Writes

Collection write operations (inserts, updates, and deletes) perform storage engine writes to both
the collection's RecordStore and relevant index's SortedDataInterface in the same storage transaction, or
WUOW. This ensures that completed, not-building indexes are always consistent with collection data.

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
[WiredTigerRecordStore::insertRecords](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/storage/wiredtiger/wiredtiger_record_store.cpp#L1494).



# Concurrency Control

Theoretically, one could design a database that used only mutexes to maintain database consistency
while supporting multiple simultaneous operations; however, this solution would result in pretty bad
performance and would a be strain on the operating system. Therefore, databases typically use a more
complex method of coordinating operations. This design consists of Resources (lockable entities),
some of which may be organized in a Hierarchy, and Locks (requests for access to a resource). A Lock
Manager is responsible for keeping track of Resources and Locks, and for managing each Resource's
Lock Queue.  The Lock Manager identifies Resources with a ResourceId.

## Resource Hierarchy

In MongoDB, Resources are arranged in a hierarchy, in order to provide an ordering to prevent
deadlocks when locking multiple Resources, and also as an implementation of Intent Locking (an
optimization for locking higher level resources). The hierarchy of ResourceTypes is as follows:

1. Global (three - see below)
1. Database (one per database on the server)
1. Collection (one per collection on the server)

Each resource must be locked in order from the top. Therefore, if a Collection resource is desired
to be locked, one must first lock the one Global resource, and then lock the Database resource that
is the parent of the Collection. Finally, the Collection resource is locked.

In addition to these ResourceTypes, there also exists ResourceMutex, which is independent of this
hierarchy.  One can use ResourceMutex instead of a regular mutex if one desires the features of the
lock manager, such as fair queuing and the ability to have multiple simultaneous lock holders.

## Lock Modes

The lock manager keeps track of each Resource's _granted locks_ and a queue of _waiting locks_.
Rather than the binary "locked-or-not" modes of a mutex, a MongoDB lock can have one of several
_modes_. Modes have different _compatibilities_ with other locks for the same resource. Locks with
compatible modes can be simultaneously granted to the same resource, while locks with modes that are
incompatible with any currently granted lock on a resource must wait in the waiting queue for that
resource until the conflicting granted locks are unlocked.  The different types of modes are:
1. X (exclusive): Used to perform writes and reads on the resource.
2. S (shared): Used to perform only reads on the resource (thus, it is okay to Share with other
   compatible locks).
3. IX (intent-exclusive): Used to indicate that an X lock is taken at a level in the hierarchy below
   this resource.  This lock mode is used to block X or S locks on this resource.
4. IS (intent-shared): Used to indicate that an S lock is taken at a level in the hierarchy below
   this resource. This lock mode is used to block X locks on this resource.

## Lock Compatibility Matrix

This matrix answers the question, given a granted lock on a resource with the mode given, is a
requested lock on that same resource with the given mode compatible?

| Requested Mode |||                  Granted Mode               |||
|:---------------|:---------:|:-------:|:-------:|:------:|:------:|
|                | MODE_NONE | MODE_IS | MODE_IX | MODE_S | MODE_X |
| MODE_IS        |     Y     |    Y    |    Y    |    Y   |   N    |
| MODE_IX        |     Y     |    Y    |    Y    |    N   |   N    |
| MODE_S         |     Y     |    Y    |    N    |    Y   |   N    |
| MODE_X         |     Y     |    N    |    N    |    N   |   N    |

Typically, locks are granted in the order they are queued, but some LockRequest behaviors can be
specially selected to break this rule. One behavior is _enqueueAtFront_, which allows important lock
acquisitions to cut to the front of the line, in order to expedite them. Currently, all mode X and S
locks for the three Global Resources (Global, RSTL, and PBWM) automatically use this option.
Another behavior is _compatibleFirst_, which allows compatible lock requests to cut ahead of others
waiting in the queue and be granted immediately; note that this mode might starve queued lock
requests indefinitely.

### Replication State Transition Lock (RSTL)

The Replication State Transition Lock is of ResourceType Global, so it must be locked prior to
locking any Database level resource. This lock is used to synchronize replica state transitions
(typically transitions between PRIMARY, SECONDARY, and ROLLBACK states).
More information on the RSTL is contained in the [Replication Architecture Guide](https://github.com/mongodb/mongo/blob/b4db8c01a13fd70997a05857be17548b0adec020/src/mongo/db/repl/README.md#replication-state-transition-lock)

### Parallel Batch Writer Mode Lock (PBWM)

The Parallel Batch Writer Mode lock is of ResourceType Global, so it must be locked prior to locking
any Database level resource. This lock is used to synchronize secondary oplog application with other
readers, so that they do not observe inconsistent snapshots of the data. Typically this is only an
issue with readers that read with no timestamp, readers at explicit timestamps can acquire this lock
in a compatible mode with the oplog applier and thus are not blocked when the oplog applier is
running.
More information on the PBWM lock is contained in the [Replication Architecture Guide.](https://github.com/mongodb/mongo/blob/b4db8c01a13fd70997a05857be17548b0adec020/src/mongo/db/repl/README.md#parallel-batch-writer-mode)

### Global Lock

The resource known as the Global Lock is of ResourceType Global.  It is currently used to
synchronize shutdown, so that all operations are finished with the storage engine before closing it.
Certain types of global storage engine operations, such as recoverToStableTimestamp(), also require
this lock to be held in exclusive mode.

### Database Lock

Any resource of ResourceType Database protects certain database-wide operations such as database
drop.  These operations are being phased out, in the hopes that we can eliminate this ResourceType
completely.

### Collection Lock

Any resource of ResourceType Collection protects certain collection-wide operations, and in some
cases also protects the in-memory catalog structure consistency in the face of concurrent readers
and writers of the catalog. Acquiring this resource with an intent lock is an indication that the
operation is doing explicit reads (IS) or writes (IX) at the document level.  There is no Document
ResourceType, as locking at this level is done in the storage engine itself for efficiency reasons.

### Document Level Concurrency Control

Each storage engine is responsible for locking at the document level.  The WiredTiger storage engine
uses MVCC (multiversion concurrency control) along with optimistic locking in order to provide
concurrency guarantees.

## Two-Phase Locking

The lock manager automatically provides _two-phase locking_ for a given storage transaction.
Two-phase locking consists of an Expanding phase where locks are acquired but not released, and a
subsequent Shrinking phase where locks are released but not acquired.  By adhering to this protocol,
a transaction will be guaranteed to be serializable with other concurrent transactions. The
WriteUnitOfWork class manages two-phase locking in MongoDB. This results in the somewhat unexpected
behavior of the RAII locking types acquiring locks on resources upon their construction but not
unlocking the lock upon their destruction when going out of scope. Instead, the responsibility of
unlocking the locks is transferred to the WriteUnitOfWork destructor.  Note this is only true for
transactions that do writes, and therefore only for code that uses WriteUnitOfWork.


# Indexes

An index is a storage engine data structure that provides efficient lookup on fields in a
collection's data set. Indexes map document fields, keys, to documents such that a full collection
scan is not required when querying on a specific field.

All user collections have a unique index on the `_id` field, which is required. The oplog and some
system collections do not have an _id index.

Also see [MongoDB Manual - Indexes](https://docs.mongodb.com/manual/indexes/).

## Index Constraints

### Unique indexes

A unique index maintains a constraint such that duplicate values are not allowed on the indexed
field(s).

To convert a regular index to unique, one has to follow the two-step process:
  * The index has to be first set to `prepareUnique` state using `collMod` command with the index
  option `prepareUnique: true`. In this state, the index will start rejecting writes introducing
  duplicate keys.
  * The `collMod` command with the index option `unique: true` will then check for the uniqueness
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

To solve this problem, the multikey state is tracked in memory, and only persisted  when it changes
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
* While holding a collection X lock, write a new index entry to the array of indexes included as
  part of a durable catalog entry. This entry has a `ready: false` component. See [Durable
  Catalog](#durable-catalog).
* Downgrade to a collection IX lock.
* Scan all documents on the collection to be indexed
  * Generate [KeyString](#keystring) keys for the indexed fields for each document
  * Periodically yield locks and storage engine snapshots
  * Insert the generated keys into the [external sorter](#the-external-sorter)
* Read the sorted keys from the external sorter and [bulk
    load](http://source.wiredtiger.com/3.2.1/tune_bulk_load.html) into the storage engine index.
    Bulk-loading requires keys to be inserted in sorted order, but builds a B-tree structure that is
    more efficiently filled than with random insertion.
* While holding a collection X lock, make a final `ready: true` write to the durable catalog.


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
* While holding a collection IX lock to allow concurrent reads and writes
    * Because writes are still accepted, new keys may appear at the end of the _side-writes_ table.
      They will be applied in subsequent steps.
* While holding a collection S lock to block concurrent writes, but not reads
* While holding a collection X lock to block all reads and writes

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

While waiting for a commit decision, primaries and secondaries continue recieving and applying new
side writes. When a quorum is reached, the current primary, under a collection X lock, will check
all index constraints. If there are errors, it will replicate an `abortIndexBuild` oplog entry. If
the index build is successful, it will replicate a `commitIndexBuild` oplog entry.

Secondaries that were not included in the commit quorum and receive a `commitIndexBuild` oplog entry
will block replication until their index build is complete.

The `commitQuorum` for a running index build may be changed by the user via the
[`setIndexCommitQuorum`](https://github.com/mongodb/mongo/blob/v6.0/src/mongo/db/commands/set_index_commit_quorum_command.cpp#L55)
server command.

See
[IndexBuildsCoordinator::_waitForNextIndexBuildActionAndCommit](https://github.com/mongodb/mongo/blob/r4.4.0-rc9/src/mongo/db/index_builds_coordinator_mongod.cpp#L632).

## Resumable Index Builds

On clean shutdown, index builds save their progress in internal idents that will be used for resuming
the index builds when the server starts up. The persisted information includes:
* [Phase of the index build](https://github.com/mongodb/mongo/blob/0d45dd9d7ba9d3a1557217a998ad31c68a897d47/src/mongo/db/resumable_index_builds.idl#L43) when it was interrupted for shutdown:
    * initialized
    * collection scan
    * bulk load
    * drain writes
* Information relevant to the phase for reconstructing the internal state of the index build at
  startup. This may include:
    * The internal state of the external sorter.
    * Idents for side writes, duplicate keys, and skipped records.

During [startup recovery](#startup-recovery), the persisted information is used to reconstruct the
in-memory state for the index build and resume from the phase that we left off in. If we fail to
resume the index build for whatever reason, the index build will restart from the beginning.

Not all incomplete index builds are resumable upon restart. The current criteria for index build
resumability can be found in [IndexBuildsCoordinator::isIndexBuildResumable()](https://github.com/mongodb/mongo/blob/0d45dd9d7ba9d3a1557217a998ad31c68a897d47/src/mongo/db/index_builds_coordinator.cpp#L375). Generally,
index builds are resumable under the following conditions:
* Storage engine is configured to be persistent with encryption disabled.
* The index build is running on a voting member of the replica set with the default [commit quorum](#commit-quorum)
  `"votingMembers"`.
* Majority read concern is enabled.

The [Recover To A Timestamp (RTT) rollback algorithm](https://github.com/mongodb/mongo/blob/04b12743cbdcfea11b339e6ad21fc24dec8f6539/src/mongo/db/repl/README.md#rollback) supports
resuming index builds interrupted at any phase. On entering rollback, the resumable
index information is persisted to disk using the same mechanism as shutdown. We resume the
index build using the startup recovery logic that RTT uses to bring the node back to a writable
state.

For improved rollback semantics, resumable index builds require a majority read cursor during
collection scan phase. Index builds wait for the majority commit point to advance before starting
the collection scan. The majority wait happens after installing the
[side table for intercepting new writes](#temporary-side-table-for-new-writes).

See [MultiIndexBlock::_constructStateObject()](https://github.com/mongodb/mongo/blob/0d45dd9d7ba9d3a1557217a998ad31c68a897d47/src/mongo/db/catalog/multi_index_block.cpp#L900)
for where we persist the relevant information necessary to resume the index build at shutdown
and [StorageEngineImpl::_handleInternalIdents()](https://github.com/mongodb/mongo/blob/0d45dd9d7ba9d3a1557217a998ad31c68a897d47/src/mongo/db/storage/storage_engine_impl.cpp#L329)
for where we search for and parse the resume information on startup.

## Single-Phase Index Builds

Index builds on empty collections replicate a `createIndexes` oplog entry. This oplog entry was used
before FCV 4.4 for all index builds, but continues to be used in FCV 4.4 only for index builds that
are considered "single-phase" and do not need to run in the background. Unlike two-phase index
builds, the `createIndexes` oplog entry is always applied synchronously on secondaries during batch
application.

See [createIndexForApplyOps](https://github.com/mongodb/mongo/blob/6ea7d1923619b600ea0f16d7ea6e82369f288fd4/src/mongo/db/repl/oplog.cpp#L176-L183).

# KeyString

The `KeyString` format is an alternative serialization format for `BSON`. In the text below,
`KeyString` may refer to values in this format, the C++ namespace of that name or the format itself.
Indexes sort keys based on their BSON sorting order. In this order all numerical values compare
according to their mathematical value. Given a BSON document `{ x: 42.0, y : "hello"}`
and an index with the compound key `{ x : 1, y : 1}`, the document is sorted as the BSON document
`{"" : 42.0, "": "hello" }`, with the actual comparison as defined by [`BSONObj::woCompare`][] and
[`BSONElement::compareElements`][]. However, these comparison rules are complicated and can be
computationally expensive, especially for numeric types as the comparisons may require conversions
and there are lots of edge cases related to range and precision. Finding a key in a tree containing
thousands or millions of key-value pairs requires dozens of such comparisons.

To make these comparisons fast, there exists a 1:1 mapping between `BSONObj` and `KeyString`, where
`KeyString` is [binary comparable](#glossary). So, for a transformation function `t` converting
`BSONObj` to `KeyString` and two `BSONObj` values `x` and `y`, the following holds:
* `x < y` ⇔ `memcmp(t(x),t(y)) < 0`
* `x > y` ⇔ `memcmp(t(x),t(y)) > 0`
* `x = y` ⇔ `memcmp(t(x),t(y)) = 0`

## Ordering

Index keys with reverse sort order (like `{ x : -1}`) have all their `KeyString` bytes negated to
ensure correct `memcmp` comparison. As a compound index can have up to 64 keys, for decoding a
`KeyString` it is necessary to know which components need to have their bytes negated again to get
the original value. The [`Ordering`] class encodes the direction of each component in a 32-bit
unsigned integer.

## TypeBits

As the integer `42`, `NumberLong(42)`, double precision `42.0` and `NumberDecimal(42.00)` all
compare equal, for conversion back from `KeyString` to `BSONObj` additional information is necessary
in the form of `TypeBits`. When decoding a `KeyString`, typebits are consumed as values with
ambiguous types are encountered.

## Use in WiredTiger indexes

For indexes other than `_id` , the `RecordId` is appended to the end of the `KeyString` to ensure
uniqueness. In older versions of MongoDB we didn't do that, but that lead to problems during
secondary oplog application and [initial sync][] where the uniqueness constraint may be violated
temporarily. Indexes store key value pairs where they key is the `KeyString`. Current WiredTiger
secondary unique indexes may have a mix of the old and new representations described below.

| Index type                   | (Key, Value)                                                                                                                           | Data Format Version            |
| ---------------------------- | -------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------ |
| `_id` index                  | (`KeyString` without `RecordId`, `RecordId` and optionally `TypeBits`)                                                                 | index V1: 6<br />index V2: 8   |
| non-unique index             | (`KeyString` with `RecordId`, optionally `TypeBits`)                                                                                   | index V1: 6<br />index V2: 8   |
| unique secondary index (new) | (`KeyString` with `RecordId`, optionally `TypeBits`)                                                                                   | index V1: 13<br />index V2: 14 |
| unique secondary index (old) | (`KeyString` with `RecordId`, optionally `TypeBits`) or<br />(`KeyString` without `RecordId`, `RecordId` and optionally `TypeBits`)    | index V1: 11<br />index V2: 12 |

The reason for the change in index format is that the secondary key uniqueness property can be
temporarily violated during oplog application (because operations may be applied out of order).
With prepared transactions, out-of-order commits would conflict with prepared transactions.

For `_id` indexes and non-unique indexes, the index data formats will be 6 and 8 for index version
V1 and V2, respectively. For unique secondary indexes, if they are of formats 13 or 14, it is
guaranteed that the indexes only store keys of `KeyString` with `RecordId`. If they are of formats
11 or 12, they may have a mix of the keys with and without `RecordId`. Users can run a `full`
validation to check if there are keys in the old format in unique secondary indexes.

## Building KeyString values and passing them around

There are three kinds of builders for constructing `KeyString` values:
* `KeyString::Builder`: starts building using a small allocation on the stack, and
  dynamically switches to allocating memory from the heap. This is generally preferable if the value
  is only needed in the scope where it was created.
* `KeyString::HeapBuilder`: always builds using dynamic memory allocation. This has advantage that
   calling the `release` method can transfer ownership of the memory without copying.
*  `KeyString::PooledBuilder`: This class allow building many `KeyString` values tightly packed into
   larger blocks. The advantage is fewer, larger memory allocations and no wasted space due to
   internal fragmentation. This is a good approach when a large number of values is needed, such as
   for index building. However, memory for a block is only released after _no_ references to that
   block remain.

The `KeyString::Value` class holds a reference to a `SharedBufferFragment` with the `KeyString` and
its `TypeBits` if any and can be used for passing around values.

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
regulate when to write a chunk of sorted data out to disk in a temporary file.

_Code spelunking starting points:_

* [_The External Sorter Classes_](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/sorter/sorter.h)

# The TTLMonitor

The TTLMonitor runs as a background job on each mongod. On a mongod primary, the TTLMonitor is responsible for removing documents expired on [TTL Indexes](https://www.mongodb.com/docs/manual/core/index-ttl/) across the mongod instance. It continuously runs in a loop that sleeps for ['ttlMonitorSleepSecs'](https://github.com/mongodb/mongo/blob/d88a892d5b18035bd0f5393a42690e705c2007d7/src/mongo/db/ttl.idl#L39) and then performs a TTL Pass to remove all expired documents.

The TTLMonitor exhibits different behavior pending on whether batched deletes are enabled. When enabled (the default), the TTLMonitor batches TTL deletions and also removes expired documents more fairly among TTL indexes. When disabled, the TTLMonitor falls back to legacy, doc-by-doc deletions and deletes all expired documents from a single TTL index before moving to the next one. The legacy behavior can lead to the TTLMonitor getting "stuck" deleting large ranges of documents on a single TTL index, starving other indexes of deletes at regular intervals.

### Fair TTL Deletion
If ['ttlMonitorBatchDeletes'](https://github.com/mongodb/mongo/blob/d88a892d5b18035bd0f5393a42690e705c2007d7/src/mongo/db/ttl.idl#L48) is specified, the TTLMonitor will batch deletes and provides fair TTL deletion as follows:
* The TTL pass consists of one or more subpasses.
* Each subpass refreshes its view of TTL indexes in the system. It removes documents on each TTL index in a round-robin fashion until there are no more expired documents or ['ttlMonitorSubPassTargetSecs'](https://github.com/mongodb/mongo/blob/d88a892d5b18035bd0f5393a42690e705c2007d7/src/mongo/db/ttl.idl#L58) is reached.
  * The delete on each TTL index removes up to ['ttlIndexDeleteTargetDocs'](https://github.com/mongodb/mongo/blob/d88a892d5b18035bd0f5393a42690e705c2007d7/src/mongo/db/ttl.idl#L84) or runs up to ['ttlIndexDeleteTargetTimeMS'](https://github.com/mongodb/mongo/blob/d88a892d5b18035bd0f5393a42690e705c2007d7/src/mongo/db/ttl.idl#L72), whichever target is met first. The same TTL index can be queued up to be revisited in the same subpass if there are outstanding deletions.
  * A TTL index is not visited any longer in a subpass once all documents are deleted.
* If there are outstanding deletions by the end of the subpass for any TTL index, a new subpass starts immediately within the same pass.

_Code spelunking starting points:_

* [_The TTLMonitor Class_](https://github.com/mongodb/mongo/blob/d88a892d5b18035bd0f5393a42690e705c2007d7/src/mongo/db/ttl.h)
* [_The TTLCollectionCache Class_](https://github.com/mongodb/mongo/blob/d88a892d5b18035bd0f5393a42690e705c2007d7/src/mongo/db/ttl_collection_cache.h)
* [_ttl.idl_](https://github.com/mongodb/mongo/blob/d88a892d5b18035bd0f5393a42690e705c2007d7/src/mongo/db/ttl.idl)

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

* Corrupt WiredTiger data files
  * Includes all collections, `_mdb_catalog`, and `sizeStorer`
* Missing WiredTiger data files
  * Includes all collections, `_mdb_catalog`, and `sizeStorer`
* Index inconsistencies
  * Validate [repair mode](#repair-mode) attempts to fix index inconsistencies to avoid a full index
    rebuild.
  * Indexes are rebuilt on collections after they have been salvaged or if they fail validation and
    validate repair mode is unable to fix all errors.
* Unsalvageable collection data files
* Corrupt metadata
    * `WiredTiger.wt`, `WiredTiger.turtle`, and WT journal files
* “Orphaned” data files
    * Collection files missing from the `WiredTiger.wt` metadata
    * Collection files missing from the `_mdb_catalog` table
    * We cannot support restoring orphaned files that are missing from both metadata sources
* Missing `featureCompatibilityVersion` document

## Repair Procedure

1. Initialize the WiredTigerKVEngine. If a call to `wiredtiger_open` returns the `WT_TRY_SALVAGE`
   error code, this indicates there is some form of corruption in the WiredTiger metadata. Attempt
   to [salvage the
   metadata](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L1046-L1071)
   by using the WiredTiger `salvage=true` configuration option.
2. Initialize the StorageEngine and [salvage the `_mdb_catalog` table, if
   needed](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/storage_engine_impl.cpp#L95).
3. Recover orphaned collections.
    * If an [ident](#glossary) is known to WiredTiger but is not present in the `_mdb_catalog`,
      [create a new
      collection](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/storage_engine_impl.cpp#L145-L189)
      with the prefix `local.orphan.<ident-name>` that references this ident.
    * If an ident is present in the `_mdb_catalog` but not known to WiredTiger, [attempt to recover
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
    *  If call to WiredTiger
       [verify()](https://source.wiredtiger.com/develop/struct_w_t___s_e_s_s_i_o_n.html#a0334da4c85fe8af4197c9a7de27467d3)
       fails, call
       [salvage()](https://source.wiredtiger.com/develop/struct_w_t___s_e_s_s_i_o_n.html#ab3399430e474f7005bd5ea20e6ec7a8e),
       which recovers as much data from a WT data file as possible.
    * If a salvage is unsuccessful, rename the data file with a `.corrupt` suffix.
    * If a data file is missing or a salvage was unsuccessful, [drop the original table from the
      metadata, and create a new, empty
      table](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L1262-L1274)
      under the original name. This allows MongoDB to continue to start up despite present
      corruption.
    * After any salvage operation, [all indexes are
      rebuilt](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/repair_database.cpp#L134-L149)
      for that collection.
5. Validate collection and index consistency
    * [Collection validation](#collection-validation) checks for consistency between the collection
      and indexes. Validate repair mode attempts to fix any inconsistencies it finds.
6. Rebuild indexes
    * If a collection's data has been salvaged or any index inconsistencies are not repairable by
      validate repair mode, [all indexes are
      rebuilt](https://github.com/mongodb/mongo/blob/4406491b2b137984c2583db98068b7d18ea32171/src/mongo/db/repair.cpp#L273-L275).
    * While a unique index is being rebuilt, if any documents are found to have duplicate keys, then
      those documents are inserted into a lost and found collection with the format
      `local.lost_and_found.<collection UUID>`.
7. [Invalidate the replica set
   configuration](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/repair_database_and_check_version.cpp#L460-L485)
   if data has been or could have been modified. This [prevents a repaired node from
   joining](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/repl/replication_coordinator_impl.cpp#L486-L494)
   and threatening the consistency of its replica set.

Additionally:
* When repair starts, it creates a temporary file, `_repair_incomplete` that is only removed when
  repair completes. The server [will not start up
  normally](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/storage_engine_init.cpp#L82-L86)
  as long as this file is present.
* Repair [will restore a
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
dropped](https://github.com/mongodb/mongo/blob/e485c1a8011d85682cb8dafa87ab92b9c23daa66/src/mongo/db/storage/storage_engine_impl.cpp#L406-L408
"Github"). A MongoDB record store that points to a WT table that doesn't exist is considered [a
fatal
error](https://github.com/mongodb/mongo/blob/e485c1a8011d85682cb8dafa87ab92b9c23daa66/src/mongo/db/storage/storage_engine_impl.cpp#L412-L425
"Github"). An index that doesn't point to a WT table is [scheduled to be
rebuilt](https://github.com/mongodb/mongo/blob/e485c1a8011d85682cb8dafa87ab92b9c23daa66/src/mongo/db/storage/storage_engine_impl.cpp#L479
"Github"). The index logic is more relaxed because indexes do not go through two-phase drop when
running with enableMajorityReadConcern=false.

The second step of recovering the catalog is [reconciling unfinished index builds](https://github.com/mongodb/mongo/blob/e485c1a8011d85682cb8dafa87ab92b9c23daa66/src/mongo/db/storage/storage_engine_impl.cpp#L427-L432
"Github"). In 4.7+ the story will simplify, but right now there are a few outcomes:
* An [unfinished FCV 4.2- background index build on the primary](https://github.com/mongodb/mongo/blob/e485c1a8011d85682cb8dafa87ab92b9c23daa66/src/mongo/db/storage/storage_engine_impl.cpp#L527-L542 "Github") will be discarded (no oplog entry
  was ever written saying the index exists).
* An [unfinished FCV 4.2- background index build on a secondary](https://github.com/mongodb/mongo/blob/e485c1a8011d85682cb8dafa87ab92b9c23daa66/src/mongo/db/storage/storage_engine_impl.cpp#L513-L525 "Github") will be rebuilt in the foreground
  (an oplog entry was written saying the index exists).
* An [unfinished FCV 4.4\+](https://github.com/mongodb/mongo/blob/e485c1a8011d85682cb8dafa87ab92b9c23daa66/src/mongo/db/storage/storage_engine_impl.cpp#L483-L511 "Github") background index build will be restarted in the background.
    * If the server was previously shut down cleanly, we may be able to [resume the index build](#resumable-index-builds)
      at the phase that it was stopped in. This resume information is stored in an internal ident
      written at shutdown. If we fail to resume the index build, we will clean up the internal ident
      and restart the index build in the background.

After storage completes its recovery, control is passed to [replication
recovery](https://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/README.md#startup-recovery
"Github"). While storage recovery is responsible for recovering the oplog to meet durability
guarantees and getting the two catalogs in sync, replication recovery takes responsibility for
getting collection data in sync with the oplog. Replication starts replaying oplog from the
`recovery_timestamp + 1`. When WiredTiger takes a checkpoint, it uses the
[`stable_timestamp`](https://github.com/mongodb/mongo/blob/87de9a0cb1/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L2011 "Github") (effectively a `read_timestamp`) for what data should be persisted in the
checkpoint. Every "data write" (collection/index contents, _mdb_catalog contents) corresponding to an oplog entry with a
timestamp <= the `stable_timestamp` will be included in this checkpoint. None of the data writes
later than the `stable_timestamp` are included in the checkpoint. When the checkpoint is completed, the
`stable_timestamp` is known as the checkpoint's [`checkpoint_timestamp`](https://github.com/mongodb/mongo/blob/834a3c49d9ea9bfe2361650475158fc0dbb374cd/src/third_party/wiredtiger/src/meta/meta_ckpt.c#L921 "Github"). When WiredTiger starts up on a checkpoint,
that checkpoint's timestamp is known as the
[`recovery_timestamp`](https://github.com/mongodb/mongo/blob/87de9a0cb1/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L684
"Github").

## Recovery To A Stable Timestamp

## Table Ident Resolution

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
* When the [stable timestamp](../repl/README.md#replication-timestamp-glossary) is greater than or
  equal to the [initial data timestamp](../repl/README.md#replication-timestamp-glossary), we take a
  stable checkpoint, which is a durable view of the data at a particular timestamp. This is for
  steady-state replication.
* The [initial data timestamp](../repl/README.md#replication-timestamp-glossary) is not set, so we
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

* [_The JournalFlusher class_](https://github.com/mongodb/mongo/blob/767494374cf12d76fc74911d1d0fcc2bbce0cd6b/src/mongo/db/storage/control/journal_flusher.h)
  * Perioidically and upon request flushes the journal to disk.
* [_Code that ultimately calls flush journal on WiredTiger_](https://github.com/mongodb/mongo/blob/767494374cf12d76fc74911d1d0fcc2bbce0cd6b/src/mongo/db/storage/wiredtiger/wiredtiger_session_cache.cpp#L241-L362)
  * Skips flushing if ephemeral mode engine; may do a journal flush or take a checkpoint depending
    on server settings.
* [_Control of whether journaling is enabled_](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h#L451)
  * 'durable' confusingly means journaling is enabled.
* [_Whether WT journals a collection_](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_util.cpp#L560-L580)

# Flow Control

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

## Ticket admission mechanism

The ticket admission Flow Control mechanism allows a specified number of global IX lock acquisitions
every second. Most global IX lock acquisitions (except for those that explicitly circumvent Flow
Control) must first acquire a "Flow Control ticket" before acquiring a ticket for the lock. When
there are no more flow control tickets available in a one second period, remaining attempts to
acquire flow control tickets stall until the next period, when the available flow control tickets
are replenished. It should be noted that there is no "pool" of flow control tickets that threads
give and take from; an independent mechanism refreshes the ticket counts every second.

When the Flow Control mechanism refreshes available tickets, it calculates how many tickets it
should allow in order to address the majority committed lag.

The Flow Control mechanism determines how many tickets to replenish every period based on:
1. The current majority committed replication lag with respect to the configured target maximum
   replication lag
1. How many operations the secondary sustaining the commit point has applied in the last period
1. How many IX locks per operation were acquired in the last period

## Configurable constants

Criterion #2 determines a "base" number of tickets to be used in the calculation. When the current
majority committed lag is greater than or equal to a certain configurable threshold percentage of
the target maximum, the Flow Control mechanism scales down this "base" number based on the
discrepancy between the two lag values. For some configurable constant 0 < k < 1, it calculates the
following:

`base * k ^ ((lag - threshold)/threshold) * fudge factor`

The fudge factor is also configurable and should be close to 1. Its purpose is to assign slightly
lower than the "base" number of tickets when the current lag is close to the threshold.  Criterion
#3 is then multiplied by the result of the above calculation to translate a count of operations into
a count of lock acquisitions.

When the majority committed lag is less than the threshold percentage of the target maximum, the
number of tickets assigned in the previous period is used as the "base" of the calculation. This
number is added to a configurable constant (the ticket "adder" constant), and the sum is multiplied
by another configurable constant (the ticket "multiplier" constant). This product is the new number
of tickets to be assigned in the next period.

When the Flow Control mechanism is disabled, the ticket refresher mechanism always allows one
billion flow control ticket acquisitions per second. The Flow Control mechanism can be disabled
explicitly via a server parameter and implicitly via setting enableMajorityReadConcern to
false. Additionally, the mechanism is disabled on nodes that cannot accept writes.

Criteria #2 and #3 are determined using a sampling mechanism that periodically stores the necessary
data as primaries process writes. The sampling mechanism executes regardless of whether Flow Control
is enabled.

## Oscillations

There are known scenarios in which the Flow Control mechanism causes write throughput to
oscillate. There is no known work that can be done to eliminate oscillations entirely for this
mechanism without hindering other aspects of the mechanism. Work was done (see SERVER-39867) to
dampen the oscillations at the expense of throughput.

## Throttling internal operations

The Flow Control mechanism throttles all IX lock acquisitions regardless of whether they are from
client or system operations unless they are part of an operation that is explicitly excluded from
Flow Control. Writes that occur as part of replica set elections in particular are excluded. See
SERVER-39868 for more details.


# Collection Validation

Collection validation is used to check both the validity and integrity of the data, which in turn
informs us whether there’s any data corruption present in the collection at the time of execution.

There are two forms of validation, foreground and background.

* Foreground validation requires exclusive access to the collection which prevents CRUD operations
from running. The benefit of this is that we're not validating a potentially stale snapshot and that
allows us to perform corrective operations such as fixing the collection's fast count.

* Background validation only uses intent locks on the collection and reads using a timestamp in
order to have a consistent view across the collection and its indexes. This mode allows CRUD
operations to be performed without being blocked. Background validation also periodically yields its
locks to allow operations that require exclusive locks to run, such as dropping the collection.

Additionally, users can specify that they'd like to perform a `full` validation.
* Storage engines run custom validation hooks on the
  [RecordStore](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/record_store.h#L445-L451)
  and
  [SortedDataInterface](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/sorted_data_interface.h#L130-L135)
  as part of the storage interface.
* These hooks enable storage engines to perform internal data structure checks that MongoDB would
  otherwise not be able to perform.
* More comprehensive and time-consuming checks will run to detect more types of non-conformant BSON
  documents with duplicate field names, invalid UTF-8 characters, and non-decompressible BSON
  Columns.
* Full validations are not compatible with background validation.

[Public docs on how to run validation and interpret the results.](https://docs.mongodb.com/manual/reference/command/validate/)

## Types of Validation
* Verifies the collection's durable catalog entry and in-memory state match.
* Indexes are marked as [multikey](#multikey-indexes) correctly.
* Index [multikey](#multikey-indexes) paths cover all of the records in the `RecordStore`.
* Indexes are not missing [multikey](#multikey-indexes) metadata information.
* Index entries are in increasing order if the sort order is ascending.
* Index entries are in decreasing order if the sort order is descending.
* Unique indexes do not have duplicate keys.
* Documents in the collection are valid and conformant `BSON`.
* Fast count matches the number of records in the `RecordStore`.
  + For foreground validation only.
* The number of _id index entries always matches the number of records in the `RecordStore`.
* The number of index entries for each index is not greater than the number of records in the record
  store.
  + Not checked for indexed arrays and wildcard indexes.
* The number of index entries for each index is not less than the number of records in the record
  store.
  + Not checked for sparse and partial indexes.
* Time-series bucket collections are valid.

## Validation Procedure
* Instantiates the objects used throughout the validation procedure.
    + [ValidateState](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/validate_state.h)
      maintains the state for the collection being validated, such as locking, cursor management
      for the collection and each index, data throttling (for background validation), and general
      information about the collection.
    + [IndexConsistency](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/index_consistency.h)
      keeps track of the number of keys detected in the record store and indexes. Detects when there
      are index inconsistencies and maintains the information about the inconsistencies for
      reporting.
    + [ValidateAdaptor](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/validate_adaptor.h)
      used to traverse the record store and indexes. Validates that the records seen are valid
      `BSON` conformant to most [BSON specifications](https://bsonspec.org/spec.html). In `full`
      and `checkBSONConformant` validation modes, all `BSON` checks, including the time-consuming
      ones, will be enabled.
* If a `full` validation was requested, we run the storage engines validation hooks at this point to
  allow a more thorough check to be performed.
* Validates the [collection’s in-memory](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/collection.h)
  state with the [durable catalog](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/durable_catalog.h#L242-L243)
  entry information to ensure there are [no mismatches](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/collection_validation.cpp#L363-L425)
  between the two.
* [Initializes all the cursors](https://github.com/mongodb/mongo/blob/07765dda62d4709cddc9506ea378c0d711791b57/src/mongo/db/catalog/validate_state.cpp#L144-L205)
  on the `RecordStore` and `SortedDataInterface` of each index in the `ValidateState` object.
    + We choose a read timestamp (`ReadSource`) based on the validation mode: `kNoTimestamp`
    for foreground validation and `kCheckpoint` for background validation.
* Traverses the `RecordStore` using the `ValidateAdaptor` object.
    + [Validates each record and adds the document's index key set to the IndexConsistency object](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/validate_adaptor.cpp#L61-L140)
      for consistency checks at later stages.
        + In an effort to reduce the memory footprint of validation, the `IndexConsistency` object
          [hashes](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/index_consistency.cpp#L307-L309)
          the keys passed in to one of many buckets.
        + Document keys will
          [increment](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/index_consistency.cpp#L204-L214)
          the respective bucket.
        + Index keys will
          [decrement](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/index_consistency.cpp#L239-L248)
          the respective bucket.
    + Checks that the `RecordId` is in [increasing order](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/validate_adaptor.cpp#L305-L308).
    + [Adjusts the fast count](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/validate_adaptor.cpp#L348-L353)
      stored in the `RecordStore` (when performing a foreground validation only).
* Traverses the index entries for each index in the collection.
    + [Validates the index key order to ensure that index entries are in increasing or decreasing order](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/validate_adaptor.cpp#L144-L188).
    + Adds the index key to the `IndexConsistency` object for consistency checks at later stages.
* After the traversals are finished, the `IndexConsistency` object is checked to detect any
  inconsistencies between the collection and indexes.
    + If a bucket has a `value of 0`, then there are no inconsistencies for the keys that hashed
      there.
    + If a bucket has a `value greater than 0`, then we are missing index entries.
    + If a bucket has a `value less than 0`, then we have extra index entries.
* Upon detection of any index inconsistencies, the [second phase of validation](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/collection_validation.cpp#L186-L240)
  is executed. If no index inconsistencies were detected, we’re finished and we report back to the
  user.
    + The second phase of validation re-runs the first phase and expands its memory footprint by
      recording the detailed information of the keys that were inconsistent during the first phase
      of validation (keys that hashed to buckets where the value was not 0 in the end).
    + This is used to [pinpoint exactly where the index inconsistencies were detected](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/index_consistency.cpp#L109-L202)
      and to report them.

## Repair Mode

Validate accepts a RepairMode flag that instructs it to attempt to fix certain index
inconsistencies. Repair mode can fix inconsistencies by applying the following remediations:
* Missing index entries
  * Missing keys are inserted into the index
* Extra index entries
  * Extra keys are removed from the index
* Multikey documents are found for an index that is not marked multikey
  * The index is marked as multikey
* Multikey documents are found that are not covered by an index's multikey paths
  * The index's multikey paths are updated
* Corrupt documents
  * Documents with invalid BSON are removed

Repair mode is used by startup repair to avoid rebuilding indexes. Repair mode may also be used on
standalone nodes by passing `{ repair: true }` to the validate command.

See [RepairMode](https://github.com/mongodb/mongo/blob/4406491b2b137984c2583db98068b7d18ea32171/src/mongo/db/catalog/collection_validation.h#L71).

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

The oplog collection can be truncated both at the front end (most recent entries) and the back end
(the oldest entries). The capped setting on the oplog collection causes the oldest oplog entries to
be deleted when new writes increase the collection size past the cap. MongoDB using the WiredTiger
storage engine with `--replSet` handles oplog collection deletion specially via a purpose built
[OplogStones](#wiredtiger-oplogstones) mechanism, ignoring the generic capped collection deletion
mechanism. The front of the oplog may be truncated back to a particular timestamp during replication
startup recovery or replication rollback.

### WiredTiger OplogStones

The WiredTiger storage engine disregards the regular capped collection deletion mechanism for the
oplog collection and instead uses `OplogStones` to improve performance by batching deletes. The
oplog is broken up into a number of stones. Each stone tracks a range of the oplog, the number of
bytes in that range, and the last (newest) entry's record ID. A new stone is created when existing
stones fill up; and the oldest stone's oplog is deleted when the oplog size exceeds its cap size
setting.

### Special Timestamps That Will Not Be Truncated

The WiredTiger integration layer's `OplogStones` implementation will stall deletion waiting for
certain significant tracked timestamps to move forward past entries in the oldest stone. This is
done for correctness. Backup pins truncation in order to maintain a consistent view of the oplog;
and startup recovery after an unclean shutdown and rollback both require oplog history back to
certain timestamps.

### Min Oplog Retention

WiredTiger `OplogStones` obey an `oplogMinRetentionHours` configurable setting. When
`oplogMinRetentionHours` is active, the WT `OplogStones` will only truncate the oplog if a stone (a
sequential range of oplog) is not within the minimum time range required to remain.

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

* Only operations from user connections collect metrics. For example, internal connections from
  other replica set members do not collect metrics.
* Metrics are only collected for a specific set of commands. Those commands override the function
  `Command::collectsResourceConsumptionMetrics()`.
* Metrics for write operations are only collected on primary nodes.
  * This includes TTL index deletions.
* All attempted write operations collect metrics. This includes writes that fail or retry internally
  due to write conflicts.
* Read operations are attributed to the replication state of a node. Read metrics are broken down
  into whether they occurred in the primary or secondary replication states.
* Index builds collect metrics. Because index builds survive replication state transitions, they
  only record aggregated metrics if the node is currently primary when the index build completes.
* Metrics are not collected on `mongos` and are not supported or tested in sharded environments.
* Storage engines other than WiredTiger do not implement metrics collection.
* Metrics are not adjusted after replication rollback.

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
    // The number of random seeks to a position on an index or collection
    cursorSeeks: 0,
    // The number of keys sorted for query operations
    keysSorted: 0,
    // The number of times an in-memory sort operation had to spill to disk
    sorterSpills: 0,
    // The number of document units returned by query operations
    docUnitsReturned: 0
  },
  // Metrics recorded while the node was SECONDARY
  secondaryMetrics: {
    docBytesRead: 0,
    docUnitsRead: 0,
    idxEntryBytesRead: 0,
    idxEntryUnitsRead: 0,
    cursorSeeks: 0,
    keysSorted: 0,
    sorterSpills: 0,
    docUnitsReturned: 0
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
* A document that specifies the clustered index configuration.
  ```
  {clusteredIndex: {key: {_id: 1}, unique: true}}
  ```
* A legacy boolean parameter for backwards compatibility with 5.0 time-series collections.
  ```
  {clusteredIndex: true}
  ```

Like a secondary TTL index, clustered collections can delete old data when created with the
`expireAfterSeconds` collection creation option.

Unlike regular collections, clustered collections do not require a separate index from cluster key
values to `RecordId`s, so they lack an index on _id. While a regular collection must access two
different tables to read or write to a document, a clustered collection requires a single table
access. Queries over the _id key use bounded collection scans when no other index is available.

## Time Series Collections

A time-series collection is a view of an internal clustered collection named
`system.buckets.<name>`, where `<name>` is the name of the time-series collection. The cluster key
values are ObjectId's.

The TTL monitor will only delete data from a time-series bucket collection when a bucket's minimum
time, _id, is past the expiration plus the bucket maximum time span (default 1 hour). This
procedure avoids deleting buckets with data that is not older than the expiration time.

For more information on time-series collections, see the [timeseries/README][].

[timeseries/README]: https://github.com/mongodb/mongo/blob/master/src/mongo/db/timeseries/README.md

## Capped clustered collections

Capped clustered collections are available internally. Unlike regular capped collections, clustered
capped collections require TTL-based deletion in lieu of size-based deletion. Because on clustered
collections the natural order is the cluster key order rather than the insertion order, capped
deletions remove the documents with lowest cluster key value, which may not be the oldest documents
inserted. In order to guarantee capped insert-order semantics the caller should insert monotonically
increasing cluster key values.

Because unlike regular capped collections, clustered collections do not need to preserve insertion
order, they allow non-serialised concurrent writes. In order to avoid missing documents while
tailing a clustered collection, the user is required to enforce visibility rules similar to the ['no
holes' point](https://github.com/mongodb/mongo/blob/r5.2.0/src/mongo/db/catalog/README.md#oplog-visibility).
Majority read concern is similarly required.

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

# Glossary
**binary comparable**: Two values are binary comparable if the lexicographical order over their byte
representation, from lower memory addresses to higher addresses, is the same as the defined ordering
for that type. For example, ASCII strings are binary comparable, but double precision floating point
numbers and little-endian integers are not.

**DDL**: Acronym for Data Description Language or Data Definition Language used generally in the
context of relational databases. DDL operations in the MongoDB context include index and collection
creation or drop, as well as `collMod` operations.

**ident**: An ident is a unique identifier given to a storage engine resource. Collections and
indexes map application-layer names to storage engine idents. In WiredTiger, idents are implemented
as tables. For example, collection idents have the form: `collection-<counter>-<random number>`.

**oplog hole**: An uncommitted oplog write that can exist with out-of-order writes when a later
timestamped write happens to commit first. Oplog holes can exist in-memory and persisted on disk.

**oplogReadTimestamp**: The timestamp used for WT forward cursor oplog reads in order to avoid
advancing past oplog holes. Tracks in-memory oplog holes.

**oplogTruncateAfterPoint**: The timestamp after which oplog entries will be truncated during
startup recovery after an unclean shutdown. Tracks persisted oplog holes.

**snapshot**: A snapshot consists of a consistent view of data in the database.  In MongoDB, a
snapshot consists of all data committed with a timestamp less than or equal to the snapshot's
timestamp.

**snapshot isolation**: A guarantee that all reads in a transaction see the same consistent snapshot
of the database, and that all writes in a transaction had no conflicts with other concurrent writes,
if the transaction commits.

**storage transaction**: A concept provided by a pluggable storage engine through which changes to
data in the database can be performed.  In order to satisfy the MongoDB pluggable storage engine
requirements for atomicity, consistency, isolation, and durability, storage engines typically use
some form of transaction. In contrast, a multi-document transaction in MongoDB is a user-facing
feature providing similar guarantees across many nodes in a sharded cluster; a storage transaction
only provides guarantees within one node.

[`BSONObj::woCompare`]: https://github.com/mongodb/mongo/blob/v4.4/src/mongo/bson/bsonobj.h#L460
[`BSONElement::compareElements`]: https://github.com/mongodb/mongo/blob/v4.4/src/mongo/bson/bsonelement.cpp#L285
[`Ordering`]: https://github.com/mongodb/mongo/blob/v4.4/src/mongo/bson/ordering.h
[initial sync]: ../repl/README.md#initial-sync

# Appendix

## Collection and Index to Table relationship

Creating a collection (record store) or index requires two WT operations that cannot be made
atomic/transactional. A WT table must be created with
[WT_SESSION::create](https://source.wiredtiger.com/develop/struct_w_t___s_e_s_s_i_o_n.html#a358ca4141d59c345f401c58501276bbb
"WiredTiger Docs") and an insert/update must be made in the \_mdb\_catalog table (MongoDB's
catalog). MongoDB orders these as such:
1. Create the WT table
1. Update \_mdb\_catalog to reference the table

Note that if the process crashes in between those steps, the collection/index creation never
succeeded. Upon a restart, the WT table is dangling and can be safely deleted.

Dropping a collection/index follows the same pattern, but in reverse.
1. Delete the table from the \_mdb\_catalog
1. [Drop the WT table](https://source.wiredtiger.com/develop/struct_w_t___s_e_s_s_i_o_n.html#adf785ef53c16d9dcc77e22cc04c87b70 "WiredTiger Docs")

In this case, if a crash happens between these steps and the change to the \_mdb\_catalog was made
durable (in modern versions, only possible via a checkpoint; the \_mdb\_catalog is not logged), the
WT table is once again dangling on restart. Note that in the absense of a history, this state is
indistinguishable from the creation case, establishing a strong invariant.

## Cherry-picked WT log Details
- The WT log is a write ahead log. Before a [transaction commit](https://source.wiredtiger.com/develop/struct_w_t___s_e_s_s_i_o_n.html#a712226eca5ade5bd123026c624468fa2 "WiredTiger Docs") returns to the application, logged writes
must have their log entry bytes written into WiredTiger's log buffer. Depending on `sync` setting,
those bytes may or may not be on disk.
- MongoDB only chooses to log writes to a subset of WT's tables (e.g: the oplog).
- MongoDB does not `sync` the log on transaction commit. But rather uses the [log
  flush](https://source.wiredtiger.com/develop/struct_w_t___s_e_s_s_i_o_n.html#a1843292630960309129dcfe00e1a3817
  "WiredTiger Docs") API. This optimization is two-fold. Writes that do not require to be
  persisted do not need to wait for durability on disk. Second, this pattern allows for batching
  of writes to go to disk for improved throughput.
- WiredTiger's log is similar to MongoDB's oplog in that multiple writers can concurrently copy
  their bytes representing a log record into WiredTiger's log buffer similar to how multiple
  MongoDB writes can concurrently generate oplog entries.
- MongoDB's optime generator for the oplog is analogous to WT's LSN (log sequence number)
  generator. Both are a small critical section to ensure concurrent writes don't get the same
  timestamp key/memory address to write an oplog entry value/log bytes into.
- While MongoDB's oplog writes are logical (the key is a timestamp), WT's are obviously more
physical (the key is a memory->disk location). WiredTiger is writing to a memory buffer. Thus before a
transaction commit can go to the log buffer to "request a slot", it must know how many bytes it's
going to write. Compare this to a multi-statement transaction replicating as a single applyOps
versus each statement generating an individual oplog entry for each write that's part of the
transaction.
- MongoDB testing sometimes uses a [WT debugging
  option](https://github.com/mongodb/mongo/blob/a7bd84dc5ad15694864526612bceb3877672d8a9/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L601
  "Github") that will write "no-op" log entries for other operations performed on a
  transaction. Such as setting a timestamp or writing to a table that is not configured to be
  written to WT's log (e.g: a typical user collection and index).

The most important WT log entry for MongoDB is one that represents an insert into the
oplog.
```
  { "lsn" : [1,57984],
    "hdr_flags" : "compressed",
    "rec_len" : 384,
    "mem_len" : 423,
    "type" : "commit",
    "txnid" : 118,
    "ops": [
		{ "optype": "row_put",
		  "fileid": 14 0xe,
		  "key": "\u00e8^\u00eat@\u00ff\u00ff\u00df\u00c2",
		  "key-hex": "e85eea7440ffffdfc2",
		  "value": "\u009f\u0000\u0000\u0000\u0002op\u0000\u0002\u0000\u0000\u0000i\u0000\u0002ns\u0000\n\u0000\u0000\u0000test.coll\u0000\u0005ui\u0000\u0010\u0000\u0000\u0000\u0004\u0017\u009d\u00b0\u00fc\u00b2,O\u0004\u0084\u00bdY\u00e9%\u001dm\u00ba\u0003o\u00002\u0000\u0000\u0000\u0007_id\u0000^\u00eatA\u00d4\u0098\u00b7\u008bD\u009b\u00b2\u008c\u0002payload\u0000\u000f\u0000\u0000\u0000data and bytes\u0000\u0000\u0011ts\u0000\u0002\u0000\u0000\u0000At\u00ea^\u0012t\u0000\u0001\u0000\u0000\u0000\u0000\u0000\u0000\u0000\twall\u0000\u0085\u001e\u00d6\u00c3r\u0001\u0000\u0000\u0012v\u0000\u0002\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000",
		  "value-bson": {
				u'ns': u'test.coll',
				u'o': {u'_id': ObjectId('5eea7441d498b78b449bb28c'), u'payload': u'data and bytes'},
				u'op': u'i',
				u't': 1L,
				u'ts': Timestamp(1592423489, 2),
				u'ui': UUID('179db0fc-b22c-4f04-84bd-59e9251d6dba'),
				u'v': 2L,
				u'wall': datetime.datetime(2020, 6, 17, 19, 51, 29, 157000)}
      }
    ]
  }
```
- `lsn` is a log sequence number. The WiredTiger log files are named with numbers as a
  suffix, e.g: `WiredTigerLog.0000000001`. In this example, the LSN's first value `1` maps to log
  file `0000000001`. The second value `57984` is the byte offset in the file.
- `hdr_flags` stands for header flags. Think HTTP headers. MongoDB configures WiredTiger to use
  snappy compression on its journal entries. Small journal entries (< 128 bytes?) won't be
  compressed.
- `rec_len` is the number of bytes for the record
- `type` is...the type of journal entry. The type will be `commit` for application's committing a
  transaction. Other types are typically for internal WT operations. Examples include `file_sync`,
  `checkpoint` and `system`.
- `txnid` is WT's transaction id associated with the log record.
- `ops` is a list of operations that are part of the transaction. A transaction that inserts two
  documents and removes a third will see three entries. Two `row_put` operations followed by a
  `row_remove`.
- `ops.fileid` refers to the WT table that the operation is performed against. The fileid mapping
  is held in the `WiredTiger.wt` file (a table within itself). This value is faked for WT's
  logging debug mode for tables which MongoDB is not logging.
- `ops.key` and `ops.value` are the binary representations of the inserted document (`value` is omitted
  for removal).
- `ops.key-hex` and `ops.value-bson` are specific to the pretty printing tool used.

[read-copy-update]: https://en.wikipedia.org/wiki/Read-copy-update
