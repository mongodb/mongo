# Storage Engine API

The purpose of the Storage Engine API is to allow for pluggable storage engines in MongoDB (refer to
the [Storage FAQ][]). This document gives a brief overview of the API, and provides pointers to
places with more detailed documentation.

Third-party storage engines are integrated through self-contained modules that can be dropped into
an existing MongoDB source tree, and will be automatically configured and included.

For more context and information on how this API is used, see the [Catalog](../catalog/README.md).

## Record Stores

A database contains one or more collections, each with a number of indexes, and a catalog listing
them. All MongoDB collections are implemented with a [RecordStore](record_store.h) and indexes are
implemented with a [SortedDataInterface](sorted_data_interface.h). By using the
[KVEngine](kv/kv_engine.h) class, you only have to deal with the abstraction, as the
[StorageEngineImpl](storage_engine_impl.h) implements the [StorageEngine](storage_engine.h)
interface, using record stores for catalogs. See the [Catalog](../catalog/README.md) for more information.

### Record Identities

A [RecordId](record_id.h) is a unique identifier, assigned by the storage engine, for a specific
document or entry in a record store at a given time. For storage engines based in the KVEngine, the
record identity is fixed, but other storage engines may change it when updating a document. Note
that changing record ids can be very expensive, as indexes map to the RecordId. A single document
with a large array may have thousands of index entries, resulting in very expensive updates.

### Spill Tables

Some operations may wish to relieve memory pressure by temporarily spilling some of their state to
disk. The [SpillTable](spill_table.h) API provides an interface to do so which, in order to support
isolation from non-spilling reads/writes, can use an entirely separate storage engine instance. It
also will automatically drop the underlying table upon its destruction. Further, writes to a spill
table will fail if the available disk space falls below a provided threshold. Note that reading
from/writing to a spill table does not support transactionality or timestamping.

## Locking and Concurrency

MongoDB uses multi-granular intent locking; see the [Concurrency FAQ][]. In all cases, this will
ensure that operations to meta-data, such as creation and deletion of record stores, are serialized
with respect to other accesses.

See the [Catalog](../catalog/README) and [Concurrency Control](../concurrency/README.md) for more information.

## Transactions

Operations use a [RecoveryUnit](recovery_unit.h), implemented by the storage engine, to provide
transaction semantics as described below.

### RecoveryUnit Lifetime

Most operations create a [RecoveryUnit](recovery_unit.h) upon creation of their OperationContext
that exists for the lifetime of the operation. Query operations that return a cursor to the client
hold an idle recovery unit as long as that client cursor, with the operation context switching
between its own recovery unit and that of the client cursor. User multi-statement transactions
switch an active recovery unit onto operation contexts used to perform transaction operations.

In some rare cases, operations may use more than one recovery unit at a time.

### RecoveryUnit

Each pluggable storage engine for MongoDB must implement `RecoveryUnit` as one of the base classes
for the storage engine API. Typically, storage engines satisfy the `RecoveryUnit` requirements with
some form of [snapshot isolation](#glossary) with transactions. Such transactions are called storage
transactions elsewhere in this document, to differentiate them from the higher-level _multi-document
transactions_ accessible to users of MongoDB. The RecoveryUnit manages the lifetime of a storage
engine [snapshot](#glossary). MongoDB does not always explicitly open snapshots, therefore
RecoveryUnits implicitly open snapshots on the first read or write operation.

### Timestamps

In MongoDB, a snapshot can be opened with or without a _timestamp_ when using a
[ReadSource](https://github.com/mongodb/mongo/blob/b2c1fa4f121fdb6cdffa924b802271d68c3367a3/src/mongo/db/storage/recovery_unit.h#L391-L421)
that uses timestamps. The snapshot will return all data committed with a timestamp less than or
equal to the snapshot's timestamp. No uncommitted data is visible in a snapshot, and data changes in
storage transactions that commit after a snapshot is created, regardless of their timestamps, are
also not visible. Generally, one uses a `RecoveryUnit` to perform transactional reads and writes by
first configuring the `RecoveryUnit` with the desired `ReadSource` and then performing the reads and
writes using operations on `RecordStore` or `SortedDataInterface`.

### Atomicity

Writes must only become visible when explicitly committed, and in that case all pending writes
become visible atomically. Writes that are not committed before the unit of work ends must be rolled
back. In addition to writes done directly through the Storage API, such as document updates and
creation of record stores, other custom changes can be registered with the recovery unit.

### Consistency

Storage engines must ensure that atomicity and isolation guarantees span all record stores, as
otherwise the guarantee of atomic updates on a document and all its indexes would be violated.

### Isolation

Storage engines must provide snapshot isolation, either through locking, through multi-version
concurrency control (MVCC) or otherwise. The first read implicitly establishes the snapshot.
Operations can always see all changes they make in the context of a recovery unit, but other
operations cannot until a successful commit.

### Durability

Once a transaction is committed, it is not necessarily durable: if, and only if the server fails, as
result of power loss or otherwise, the database may recover to an earlier point in time. However,
atomicity of transactions must remain preserved. Similarly, in a replica set, a primary that becomes
unavailable may need to roll back to an earlier state when rejoining the replica set, if its changes
were not yet seen by a majority of nodes. The RecoveryUnit implements methods to allow operations to
wait for their committed transactions to become durable.

A transaction may become visible to other transactions as soon as it commits, and a storage engine
may use a group commit, bundling a number of transactions to achieve durability. Alternatively, a
storage engine may wait for durability at commit time.

### Write Conflicts

Systems with optimistic concurrency control (OCC) or multi-version concurrency control (MVCC) may
find that a transaction conflicts with other transactions, that executing an operation would result
in deadlock or violate other resource constraints. In such cases the storage engine may throw a
WriteConflictException to signal the transient failure. MongoDB will handle the exception, abort and
restart the transaction.

### Point-in-time snapshot reads

Two functions on the RecoveryUnit help storage engines implement point-in-time reads:
`setTimestamp/setCommitTimestamp()` and `setTimestampReadSource()`. `setTimestamp()` is used by
write transactions to label any forthcoming writes with a timestamp. Future readers can then use a
`ReadSource` before starting any reads or writes to only see writes. The storage engine must produce
the effect of reading from a snapshot that includes only writes with timestamps at or earlier than
the selectSnapshot timestamp. This means that a point-in-time read may slice across prior write
transactions by hiding only some data from a given write transaction, if that transaction had a
different timestamp set prior to each write it did.

## Classes to implement

A storage engine should generally implement the following classes. See their definitions for more
details.

- [KVEngine](kv/kv_engine.h)
- [RecordStore](record_store.h)
- [RecoveryUnit](recovery_unit.h)
- [SeekableRecordCursor](record_store.h)
- [SortedDataInterface](sorted_data_interface.h)
- [ServerStatusSection](../commands/server_status.h)

[Concurrency FAQ]: http://docs.mongodb.org/manual/faq/concurrency/
[Storage FAQ]: http://docs.mongodb.org/manual/faq/storage

# WriteUnitOfWork

A `WriteUnitOfWork` is the mechanism to control how writes are transactionally performed on the
storage engine. All the writes (and reads) performed within its scope are part of the same storage
transaction. After all writes have been staged, caller must call `commit()` in order to atomically
commit the transaction to the storage engine. It is illegal to perform writes outside the scope of a
WriteUnitOfWork since there would be no way to commit them. If the `WriteUnitOfWork` falls out of
scope before `commit()` is called, the storage transaction is rolled back and all the staged writes
are lost. Reads can be performed outside of a `WriteUnitOfWork` block; storage transactions outside
of a `WriteUnitOfWork` are always rolled back, since there are no writes to commit.

The WriteUnitOfWork has a [`groupOplogEntries` option](https://github.com/mongodb/mongo/blob/fa32d665bd63de7a9d246fa99df5e30840a931de/src/mongo/db/storage/write_unit_of_work.h#L67)
to replicate multiple writes transactionally. This option uses the [`BatchedWriteContext` class](https://github.com/mongodb/mongo/blob/9ab71f9b2fac1e384529fafaf2a819ce61834228/src/mongo/db/batched_write_context.h#L46)
to stage writes and to generate a single applyOps entry at commit, similar to what multi-document
transactions do via the [`TransactionParticipant` class](https://github.com/mongodb/mongo/blob/219990f17695b0ea4695f827a42a18e012b1e9cf/src/mongo/db/transaction/transaction_participant.h#L82).
Unlike a multi-document transaction, the applyOps entry lacks the `lsId` and the `txnNumber`
fields. Callers must ensure that the WriteUnitOfWork does not generate more than 16MB of oplog,
otherwise the operation will fail with `TransactionTooLarge` code.

As of MongoDB 6.0, the `groupOplogEntries` mode is only used by the [BatchedDeleteStage](../exec/classic/batched_delete_stage.h)
for efficient mass-deletes.

See
[WriteUnitOfWork](https://github.com/mongodb/mongo/blob/fa32d665bd63de7a9d246fa99df5e30840a931de/src/mongo/db/storage/write_unit_of_work.h).

## Lazy initialization of storage transactions

Note that storage transactions on WiredTiger are not started at the beginning of a `WriteUnitOfWork`
block. Instead, the transaction is started implicitly with the first read or write operation. To
explicitly start a transaction, one can use `RecoveryUnit::preallocateSnapshot()`.

## Changes

One can register a `Change` on a `RecoveryUnit` while in a `WriteUnitOfWork`. This allows extra
actions to be performed based on whether a `WriteUnitOfWork` commits or rolls back. These actions
will typically update in-memory state to match what was written in the storage transaction, in a
transactional way. Note that `Change`s are not executed until the destruction of the
`WriteUnitOfWork`, which can be long after the storage engine committed. Two-phase locking ensures
that all locks are held while a Change's `commit()` or `rollback()` function runs.

# StorageUnavailableException

`StorageUnavailableException` indicates that a storage transaction rolled back due to resource
contention in the storage engine. This exception is the base of exceptions related to concurrency
(`WriteConflict`) and to those related to cache pressure (`TemporarilyUnavailable` and
`TransactionTooLargeForCache`).

We recommend using the
[writeConflictRetry](https://github.com/mongodb/mongo/blob/9381db6748aada1d9a0056cea0e9899301e7f70b/src/mongo/db/concurrency/exception_util.h#L140)
helper which transparently handles all exceptions related to this error category.

## WriteConflictException

Writers may conflict with each other when more than one operation stages an uncommitted write to the
same document concurrently. To force one or more of the writers to retry, the storage engine may
throw a WriteConflictException at any point, up to and including the call to commit(). This is
referred to as optimistic concurrency control because it allows un-contended writes to commit
quickly. Because of this behavior, most WUOWs are enclosed in a writeConflictRetry loop that retries
the write transaction until it succeeds, accompanied by a bounded exponential back-off.

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
[TemporarilyUnavailableException](https://github.com/mongodb/mongo/blob/c799851554dc01493d35b43701416e9c78b3665c/src/mongo/db/concurrency/temporarily_unavailable_exception.h#L39-L45).

## TransactionTooLargeForCacheException

A TransactionTooLargeForCacheException may be thrown inside the server to indicate that an operation
was rolled-back and is unlikely to ever complete because the storage engine cache is insufficient,
even in the absence of concurrent operations. This is determined by a simple heuristic wherein,
after a rollback, a threshold on the proportion of total dirty cache bytes the running transaction
can represent and still be considered fullfillable is checked. The threshold can be tuned with the
`transactionTooLargeForCacheThreshold` parameter. Setting this threshold to its maximum value (1.0)
causes the check to be skipped and TransactionTooLargeForCacheException to be disabled.

On replica sets, if an operation succeeds on a primary, it should also succeed on a secondary. It
would be possible to convert to both TemporarilyUnavailableException and WriteConflictException, as
if TransactionTooLargeForCacheException was disabled. But on secondaries the only difference between
the two is the rate at which the operation is retried. Hence, TransactionTooLargeForCacheException
is always converted to a WriteConflictException, which retries faster, to avoid stalling replication
longer than necessary.

Prior to 6.3, or when TransactionTooLargeForCacheException is disabled, multi-document transactions
always return a WriteConflictException, which may result in drivers retrying an operation
indefinitely. For non-multi-document operations, there is a limited number of retries on
TemporarilyUnavailableException, but it might still be beneficial to not retry operations which are
unlikely to complete and are disruptive for concurrent operations.

# Idents

An ident is a unique identifier given to a storage engine resource. Collections and indexes map
application-layer names to storage engine idents. In WiredTiger, idents are implemented as tables
and, each with a `.wt` file extension.

Format of idents in the WiredTiger storage engine:

- collection idents: `collection-<unique identifier>`
- index idents: `index-<unique identifier>`
- (v8.2+) the `<unique identifier>` is created by generating a new `UUID`.

  - `collection-d3575067-0cd9-4239-a9e8-f6af884fc6fe`
  - `index-a22eca47-c9e1-4df4-a043-d10e4cd45b40`

Idents created in earlier versions of the server (pre v8.2) use a `<counter> + <random number>` combination as the `<unique identifier>` (e.g: `index-62-2245557986372974053`). Future versions of the server must continue to recognize both formats.

Server flags that alter the form of idents (this applies to indexes as well):

- `--directoryperdb`: `<db name>/collection-<unique identifier>`
- `--wiredTigerDirectoryForIndexes`: `collection/<unique identifier>`
- (both of the above): `<db name>/collection/<unique identifier>`

# Startup Recovery

There are three components to startup recovery. The first step, of course, is starting the storage
engine. More detail about WiredTiger's startup recovery procedure can be found
[here](wiredtiger/README.md#startup-recovery).

The other two parts of storage startup recovery bring the [catalog](../catalog/README.md) back into
a consistent state. The catalog typically refers to MongoDB's notion of collections and indexes, but
it's important to note that storage engines such as WiredTiger have their own notion of a catalog.

The first step of recovering the catalog is to bring MongoDB's catalog in line with the storage
engine's. This is called reconciliation. Except for rare cases, every MongoDB collection is a
RecordStore and a list of indexes (aka SortedDataInterface). Every record store and index maps to
their own ident. [The WiredTiger README](wiredtiger/README.md) describes the relationship between
creating/dropping a collection and the underlying creation/deletion of a table which justifies the
following logic. In short, the following logic is necessary because not all storage engines can
create and drop idents transactionally. When reconciling, every ident that is not "pointed to" by a
MongoDB record store or index [gets
dropped](https://github.com/mongodb/mongo/blob/6c9adc9a2d518fa046c7739e043a568f9bee6931/src/mongo/db/storage/storage_engine_impl.cpp#L663-L676 "Github"). A MongoDB record store that points to an ident that doesn't exist is considered [a fatal
error](https://github.com/mongodb/mongo/blob/6c9adc9a2d518fa046c7739e043a568f9bee6931/src/mongo/db/storage/storage_engine_impl.cpp#L679-L693 "Github"). An index that doesn't point to an ident is [ignored and
logged](https://github.com/mongodb/mongo/blob/6c9adc9a2d518fa046c7739e043a568f9bee6931/src/mongo/db/storage/storage_engine_impl.cpp#L734-L746 "Github") because there are certain cases where the catalog entry may reference an index ident which
is no longer present, such as when an unclean shutdown occurs before a checkpoint is taken during
startup recovery.

The second step of recovering the catalog is [reconciling unfinished index
builds](https://github.com/mongodb/mongo/blob/6c9adc9a2d518fa046c7739e043a568f9bee6931/src/mongo/db/storage/storage_engine_impl.cpp#L695-L699 "Github"), that could have different outcomes:

- An [index build with a
  UUID](https://github.com/mongodb/mongo/blob/6c9adc9a2d518fa046c7739e043a568f9bee6931/src/mongo/db/storage/storage_engine_impl.cpp#L748-L751 "Github") is an unfinished two-phase build and must be restarted, unless we are [resuming
  it](#resumable-index-builds). This resume information is stored in an internal ident written at
  (clean) shutdown. If we fail to resume the index build, we will clean up the internal ident and
  restart the index build in the background.
- An [unfinished index build on
  standalone](https://github.com/mongodb/mongo/blob/6c9adc9a2d518fa046c7739e043a568f9bee6931/src/mongo/db/storage/storage_engine_impl.cpp#L792-L794 "Github") will be discarded (no oplog entry was ever written saying the index exists).

After storage completes its recovery, control is passed to [replication
recovery](../repl/README.md#startup-recovery). While storage recovery is responsible for recovering
the oplog to meet durability guarantees and getting the two catalogs in sync, replication recovery
takes responsibility for getting collection data in sync with the oplog. Replication starts
replaying oplog from the [recovery
timestamp](https://github.com/mongodb/mongo/blob/9d3db5a56a6163d4aefd77997784fed21cb2d50a/src/mongo/db/storage/storage_engine.h#L545C40-L545C51).

See the [WiredTiger README](wiredtiger/README.md#checkpoints) for more details.

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

Storage engines need to [support
checkpoints](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/storage_engine.h#L267)
for MongoDB to take advantage of this, otherwise MongoDB will act as an ephemeral data store. The
frequency of these checkpoints is determined by the ['storage.syncPeriodSecs' or
'syncdelay'](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/mongod_options_storage.idl#L86-L93)
options.

# Journaling

MongoDB provides write durability via a persisted change log for replicated writes and persistence
of non-replicated writes. The replicated change log and non-replicated collections in WiredTiger are
journaled, i.e. written out to disk. The user writes themselves, however, on a --replSet server, do
not need to be written out to disk to guarantee persistence on the server.

All replicated server writes have matching change log entries representing the changes done. The
change log is stored in the `local.oplog.rs` namespace, which is set up as a capped collection so
that old unneeded log entries are eventually removed. Replication uses the oplog collection to copy
data from one server to another.

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

See the [WiredTiger README](wiredtiger/README.md#Cherry-picked-WT-log-Details) for more details on
the implementation.

Code Links:

- [_The JournalFlusher
  class_](https://github.com/mongodb/mongo/blob/767494374cf12d76fc74911d1d0fcc2bbce0cd6b/src/mongo/db/storage/control/journal_flusher.h)
  - Periodically and upon request flushes the journal to disk.

# Fast Truncation on Internal Collections

Logical deletes aren't always performant enough to keep up with inserts. To solve this, several
internal collections use `CollectionTruncateMarkers` for fast, unreplicated and untimestamped
[truncation](http://source.wiredtiger.com/1.4.2/classwiredtiger_1_1_session.html#a80a9ee8697a61a1ad13d893d67d981bb)
of expired data, in lieu of logical document deletions.

## CollectionTruncateMarkers

CollectionTruncateMarkers are an in-memory tracking mechanism to support ranged truncates on a
collection.

A collection is broken up into a number of truncate markers. Each truncate marker tracks a range in
the collection. Newer entries not captured by a truncate marker are tracked by an in-progress
"partial marker".

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

1. An insert causes the in-progress "partial marker" segment to contain more than the minimum bytes
   needed for a truncate marker.
   - The record inserted serves as the 'last record' of the newly created marker.
2. Partial marker expiration is supported, and an explicit call is made to transform the "partial
   marker" into a complete truncate marker.
   - Partial marker expiration is supported for change stream collections and ensures that expired
     documents in a partial marker will eventually be truncated - even if writes to the namespace
     cease and the partial marker never meets the minimum bytes requirement.

### Requirements & Properties

CollectionTruncateMarkers support collections that meet the following requirements:

- Insert and truncate only. No updates or individual document deletes.
- [Clustered](../catalog/README.md#clustered-collections) with no secondary indexes.
- RecordId's in Timestamp order.
- Deletion of content follows RecordId ordering.
  - This is a general property of clustered capped collections.

Collections who use CollectionTruncateMarkers share the following properties:

- Fast counts aren't expected to be accurate.
  - Truncates don't track the count and size of documents truncated in exchange for performance
    gains.
  - Markers are a best effort way to keep track of the size metrics and when to truncate expired
    data.
- Collections aren't expected to be consistent between replica set members.
  - Truncates are unreplicated, and nodes may truncate ranges at different times.
- No snapshot read concern support (ex:
  [SERVER-78296](https://jira.mongodb.org/browse/SERVER-78296)).
  - Deleting with untimestamped, unreplicated range truncation means point-in-time reads may see
    inconsistent data.

Each collection utilizing CollectionTruncateMarkers must implement its [own
policy](https://github.com/mongodb/mongo/blob/r7.1.0-rc3/src/mongo/db/storage/collection_truncate_markers.h#L277)
to determine when there are excess markers and it is time for truncation.

### In-Memory Initialization

At or shortly after startup, an initial set of CollectionTruncateMarkers are created for each
collection. The collection is either scanned or sampled to generate initial markers. Initial
truncate markers are best effort, and may hold incorrect estimates about the number of documents and
bytes within each marker. Eventually, once the initial truncate markers expire, per truncate marker
metrics will converge closer to the correct values.

### Collections that use CollectionTruncateMarkers

- The oplog - `OplogTruncateMarkers`.
- [Change stream pre images collections](#pre-images-collection-truncation) -
  `PreImagesTruncateMarkersPerNsUUID`

Read about the WiredTiger implementation of Oplog Truncation Markers [here](wiredtiger/README.md#oplog-truncation).

### Change Stream Collection Truncation

Change stream collection that uses CollectionTruncateMarkers

- pre-images: `<tenantId>_config.system.preimages` in serverless, `config.system.preimages` in
  dedicated environments.

The change stream pre-images collections has a periodic remover thread
([ChangeStreamExpiredPreImagesRemover](https://github.com/mongodb/mongo/blob/r7.1.0-rc3/src/mongo/db/pipeline/change_stream_expired_pre_image_remover.cpp#L71).
The remover thread:

1. Creates the tenant's initial CollectionTruncateMarkers for the tenant if they do not yet exist
   - Lazy initialization of the initial truncate markers is imperative so writes aren't blocked on
     startup
2. Iterates through each truncate marker. If a marker is expired, issues a truncate of all records
   older than the marker's last record, and removes the marker from the set.

#### Cleanup After Unclean Shutdown

After an unclean shutdown, all expired pre-images are truncated at startup. WiredTiger truncate
cannot guarantee a consistent view of previously truncated data on unreplicated, untimestamped
ranges after a crash. Unlike the oplog, the change stream collections aren't logged, don't persist
any special timestamps, and it's possible that previously truncated documents can resurface after
shutdown.

#### Pre Images Collection Truncation

Each tenant has 1 pre-images collection. Each pre-images collection contains pre-images across all
the tenant's pre-image enabled collections.

A pre-images collection is clustered by
[ChangeStreamPreImageId](https://github.com/mongodb/mongo/blob/r7.1.0-rc3/src/mongo/db/pipeline/change_stream_preimage.idl#L69),
which implicitly orders pre-images first by their `'nsUUID'` (the UUID of the collection the
pre-image is from), their `'ts'` (the timestamp associated with the pre-images oplog entry), and
then by their `'applyOpsIndex'` (the index into the applyOps oplog entry which generated the
pre-image, 0 if the pre-image isn't from an applyOps oplog entry).

There is a set of CollectionTruncateMarkers for each 'nsUUID' within a tenant's pre-images
collection, `PreImagesTruncateMarkersPerNsUUID`.

In a serverless environment, each tenant has a set 'expireAfterSeconds' parameter. An entry is
expired if the 'wall time' associated with the pre-image is more than 'expireAfterSeconds' older
than the node's current wall time.

In a dedicated environment, a pre-image is expired if either (1) 'expireAfterSeconds' is set and the
pre-image is expired by it or (2) its 'ts' is less than or equal to the oldest oplog entry
timestamp.

For each tenant, `ChangeStreamExpiredPreImagesRemover` iterates over each set of
`PreImagesTruncateMarkersPerNsUUID`, and issues a ranged truncate from the truncate marker's last
record to the the minimum RecordId for the nsUUID when there is an expired truncate marker.

### Code spelunking starting points:

- [The CollectionTruncateMarkers
  class](https://github.com/mongodb/mongo/blob/r7.1.0-rc3/src/mongo/db/storage/collection_truncate_markers.h#L78)
  - The main api for CollectionTruncateMarkers.
- [The OplogTruncateMarkers
  class](https://github.com/mongodb/mongo/blob/r7.1.0-rc3/src/mongo/db/storage/wiredtiger/wiredtiger_record_store_oplog_truncate_markers.h)
  - Oplog specific truncate markers.
- [The PreImagesTruncateMarkersPerNsUUID
  class](https://github.com/mongodb/mongo/blob/r7.1.0-rc3/src/mongo/db/change_stream_pre_images_truncate_markers_per_nsUUID.h#L62)
  - Truncate markers for a given nsUUID captured within a pre-images collection.
- [The PreImagesTruncateManager
  class](https://github.com/mongodb/mongo/blob/r7.1.0-rc3/src/mongo/db/change_stream_pre_images_truncate_manager.h#L70)
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
| Writer B | commit transaction | T1,T2,T3,T4,T6 are visible to new readers    |
| Reader X | open transaction   | gets a snapshot of T1-T4 and T6              |
| Writer A | commit transaction | T1,T2,T3,T4,T5,T6 are visible to new readers |
| Reader X | close transaction  | returns T1,T2,T3,T4,T6, missing T5           |

The in-memory 'no holes' point of the oplog is tracked in order to avoid data inconsistency across
replica set members. The 'oplogReadTimestamp' tracks the in-memory no holes point and is continually
advanced as new oplog writes occur and holes disappear. Forward cursor oplog readers without a
specified timestamp set at which to read (secondary callers) will automatically use the
`oplogReadTimestamp` to avoid missing entries due to oplog holes. This is essential for secondary
replica set members querying the oplog of their sync source so they do not miss any oplog entries:
subsequent `getMores` will fetch entries as they become visible without any holes behind them.
Backward cursor oplog readers bypass the oplog visibility rules to see the latest oplog entries,
disregarding any oplog holes.

# DiskSpaceMonitor

The `DiskSpaceMonitor` is a `ServiceContext` decoration that monitors available disk space every second in the database path and executes registered actions when disk space falls below specified thresholds. The `DiskSpaceMonitor` is started during MongoDB initialization.

Actions are registered with a threshold function which should return the number of threshold bytes and a action function. When the available disk space <= the number of threshold bytes, we perform the action function. Each action receives a unique ID for deregistration or to run specific actions as needed. Actions can be run by its unique ID (`runAction`) or collectively (`runAllActions`).

An example of a use of the `DiskSpaceMonitor` is the `IndexBuildsCoordinator` registers actions to kill index builds when disk space is low when neither `directoryPerDb` nor `directoryForIndexes` is enabled.

# Glossary

**oplog hole**: An uncommitted oplog write that can exist with out-of-order writes when a later
timestamped write happens to commit first. Oplog holes can exist in-memory and persisted on disk.

**oplogReadTimestamp**: The timestamp used for WT forward cursor oplog reads in order to avoid
advancing past oplog holes. Tracks in-memory oplog holes.

**snapshot**: A snapshot consists of a consistent view of data in the database. When a snapshot is
opened with a timestamp, snapshot only shows data committed with a timestamp less than or equal
to the snapshot's timestamp.

**snapshot isolation**: A guarantee that all reads in a transaction see the same consistent snapshot
of the database. Reads with snapshot isolation are repeatable, only see committed data, and never
return newly-committed data. The storage engine must provide concurrency control to resolve
concurrent writes to the same data.
