# Execution Internals
_intro to execution goes here_

# The Catalog

## In-Memory Catalog

### Collection Catalog
include discussion of RecordStore interface

### Index Catalog
include discussion of SortedDataInferface interface

### Versioning
in memory versioning (or lack thereof) is separate from on disk

#### The Minimum Visible Snapshot

## Durable Catalog
Discuss what the catalog looks like on disk -- e.g. just another WT data table we structure
specially

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


### Catalog Data Formats
What do the catalog documents look like? Are there catalog concepts constructed only in-memory and not on disk?

#### Collection Data Format

#### Index Data Format

### Versioning
e.g. data changes in tables are versioned, dropping/creating tables is not versioned

## Catalog Changes
How are updates to the catalog done in-memory and on disk?

## Two-Phase Collection and Index Drop
First phase removes access to the collection/index in the in-memory catalog, second phase drops the WT table.
Explain this is necessary because WT versions document writes but not table drops.
Necessary to support ongoing queries, repl ops like rollback, and our own startup recovery.

# Storage Transactions
Clarify transaction refers to storage engine transactions, not repl or sharding, throughout this document.

Include a discussion on how RecoveryUnit implements isolation and transactional behaviors, including ‘read source’ and how those implement read concern levels.
Maybe include a discussion of how MongoDB read concerns translate into particular read sources and data views.

## WiredTiger Snapshot

## MongoDB Point-in-Time Read

# Read Operations
How does a read work?

## Collection Read
how it works, what tables

## Index Read
_could pull out index reads and writes into its own section, if preferable_

how it works, goes from index table to collection table -- two lookups

# Write Operations
an overview of how writes (insert, update, delete) are processed

## Index Writes
_could pull out index reads and writes into its own section, if preferable_

how index tables also get updated when a write happens, (numIndexes + 1) writes total

## Vectored Insert

# Concurrency Control
We have the catalog described above; now how do we protect it?

## Lock Modes

## Lock Granularity
Different storage engines can support different levels of granularity.

### Lock Acquisition Order
discuss lock acquisition order

mention risk of deadlocks motivation

### Replication State Transition Lock (RSTL)

### Parallel Batch Writer Mode Lock (PBWM)

### Global Lock

### Database Lock

### Collection Lock

### Document Level Concurrency Control
Explain WT's optimistic concurrency control, and why we do not need document locks in the MongoDB layer.

### Mutexes

### FCV Lock

## Two-Phase Locking
We use this for transactions? Explain.

## Replica Set Transaction Locking
TBD: title of this section -- there is some confusion over what terminology will be best understood
Stashing and unstashing locks for replica set level transactions across multiple statements.
Read's IS locks are converted to IX locks in replica set transactions.

## Locking Best Practices

### Network Calls
i.e., never hold a lock across a network call unless absolutely necessary

### Long Running I/O
i.e., don't hold a lock across journal flushing

### FCV Lock Usage

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
data structures without blocking reads or writes for extended periods of time. This is acheived by
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

A primary will not commit an index build until a minimum number of data-bearing nodes have completed
the index build and are ready to commit. This threshold is called the _commit quorum_.

A `commitQuorum` option can be provided to the `createIndexes` command and specifies the number of
nodes, including itself, a primary must wait to be ready before committing. The `commitQuorum`
option accepts the same range of values as the writeConcern `"w"` option. This can be an integer
specifying the number of nodes, `"majority"`, `"votingMembers"`, or a replica set tag. The default value
is `"votingMembers"`, or all voting data-bearing nodes.

Nodes (both primary and secondary) submit votes to the primary when they have finished scanning all
data on a collection and performed the first drain of side-writes. Voting is implemented by a
`voteCommitIndexBuild` command, and is persisted as a write to the replicated
`config.system.indexBuilds` collection.

While waiting for a commit decision, primaries and secondaries continue receiving and applying new
side writes. When a quorum is reached, the current primary, under a collection X lock, will check
all index constraints. If there are errors, it will replicate an `abortIndexBuild` oplog entry. If
the index build is successful, it will replicate a `commitIndexBuild` oplog entry.

Secondaries that were not included in the commit quorum and recieve a `commitIndexBuild` oplog entry
will block replication until their index build is complete.

See
[IndexBuildsCoordinator::_waitForNextIndexBuildActionAndCommit](https://github.com/mongodb/mongo/blob/r4.4.0-rc9/src/mongo/db/index_builds_coordinator_mongod.cpp#L632).

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

| Index type                   | Key                            | Value                                |
| ---------------------------- | ------------------------------ | ------------------------------------ |
| `_id` index                  | `KeyString` without `RecordId` | `RecordId` and optionally `TypeBits` |
| non-unique index             | `KeyString` with `RecordId`    | optionally `TypeBits`                |
| unique secondary index (new) | `KeyString` with `RecordId`    | optionally `TypeBits`                |
| unique secondary index (old) | `KeyString` without `RecordId` | `RecordId` and opt. `TypeBits`       |

The reason for the change in index format is that the secondary key uniqueness property can be
temporarily violated during oplog application (because operations may be applied out of order).
With prepared transactions, out-of-order commits would conflict with prepared transactions.

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

# Repair

Data corruption has a variety of causes, but can usually be attributed to misconfigured or unreliable I/O subsystems that do not make data durable when called upon, often in the event of power outages.

MongoDB provides a command-line `--repair` utility that attempts to recover as much data as possible from an installation that fails to start up due to data corruption.

- [Types of Corruption](#types-of-corruption)
- [Repair Procedure](#repair-procedure)

## Types of Corruption

MongoDB repair attempts to address the following forms of corruption:

* Corrupt WiredTiger data files
  * Includes all collections, `_mdb_catalog`, and `sizeStorer`
* Missing WiredTiger data files
  * Includes all collections, `_mdb_catalog`, and `sizeStorer`
* Indexes
  * Prior to 4.4, all indexes were always rebuilt on all collections, even if not missing or corrupt.
  * Starting in 4.4, indexes are only rebuilt on collections that are salvaged or fail validation with inconsistencies. See [repairCollections](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/repair_database.cpp#L115).
* Unsalvageable collection data files
* Corrupt metadata
    * `WiredTiger.wt`, `WiredTiger.turtle`, and WT journal files
* “Orphaned” data files
    * Collection files missing from the `WiredTiger.wt` metadata
    * Collection files missing from the `_mdb_catalog` table
    * We cannot support restoring orphaned files that are missing from both metadata sources
* Missing `featureCompatibilityVersion` document

## Repair Procedure

1. Initialize the WiredTigerKVEngine. If a call to `wiredtiger_open` returns the `WT_TRY_SALVAGE` error code, this indicates there is some form of corruption in the WiredTiger metadata. Attempt to [salvage the metadata](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L1046-L1071) by using the WiredTiger `salvage=true` configuration option.
2. Initialize the StorageEngine and [salvage the `_mdb_catalog` table, if needed](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/storage_engine_impl.cpp#L95).
3. Recover orphaned collections.
    * If an [ident](#glossary) is known to WiredTiger but is not present in the `_mdb_catalog`,
    [create a new collection](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/storage_engine_impl.cpp#L145-L189)
    with the prefix `local.orphan.<ident-name>` that references this ident.
    * If an ident is present in the `_mdb_catalog` but not known to WiredTiger, [attempt to recover the ident](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/storage_engine_impl.cpp#L197-L229). This [procedure for orphan recovery](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L1525-L1605) is a less reliable and more invasive. It involves moving the corrupt data file to a temporary file, creates a new table with the same name, replaces the original data file over the new one, and [salvages](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L1525-L1605) the table in attempt to reconstruct the table.
4. [Verify collection data files](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L1195-L1226), and salvage if necessary.
    *  If call to WiredTiger [verify()](https://source.wiredtiger.com/develop/struct_w_t___s_e_s_s_i_o_n.html#a0334da4c85fe8af4197c9a7de27467d3) fails, call [salvage()](https://source.wiredtiger.com/develop/struct_w_t___s_e_s_s_i_o_n.html#ab3399430e474f7005bd5ea20e6ec7a8e), which recovers as much data from a WT data file as possible.
    * If a salvage is unsuccessful, rename the data file with a `.corrupt` suffix.
    * If a data file is missing or a salvage was unsuccessful, [drop the original table from the metadata, and create a new, empty table](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L1262-L1274) under the original name. This allows MongoDB to continue to start up despite present corruption.
    * After any salvage operation, [all indexes are rebuilt](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/repair_database.cpp#L134-L149) for that collection.
5. Validate collection and index consistency.
    * [Collection validation](#collection-validation) checks for consistency between the collection and indexes. If any inconsistencies are found, [all indexes are rebuilt](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/repair_database.cpp#L167-L184).
6. [Invalidate the replica set configuration](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/repair_database_and_check_version.cpp#L460-L485) if data has been or could have been modified. This [prevents a repaired node from joining](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/repl/replication_coordinator_impl.cpp#L486-L494) and threatening the consisency of its replica set.

Additionally:
* When repair starts, it creates a temporary file, `_repair_incomplete` that is only removed when repair completes. The server [will not start up normally](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/storage_engine_init.cpp#L82-L86) as long as this file is present.
* Repair [will restore a missing](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/repair_database_and_check_version.cpp#L434) `featureCompatibilityVersion` document in the `admin.system.version` to the lower FCV version available.

# Startup Recovery
How the different storage engines startup and recovery

Any differences for standalone mode?

## Recovery To A Stable Timestamp

## Table Ident Resolution

# File-System Backups

## How To Take a Backup

## How To Use Backed Up Datafiles
describe the different ways backed up datafiles can be used

explain how datafiles persist a machine’s identity which must be manipulated for some kinds of restores

## Replica Set Backup

## Sharding Backup

## Queryable Backup (Read-Only)

# Checkpoints

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
What it does (motivation). How does it do it? Ticketing.

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
* Documents in the collection are valid `BSON`.
* Fast count matches the number of records in the `RecordStore`.
  + For foreground validation only.
* The number of _id index entries always matches the number of records in the `RecordStore`.
* The number of index entries for each index is not greater than the number of records in the record
  store.
  + Not checked for indexed arrays and wildcard indexes.
* The number of index entries for each index is not less than the number of records in the record
  store.
  + Not checked for sparse and partial indexes.

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
      `BSON` and not corrupted.
* If a `full` validation was requested, we run the storage engines validation hooks at this point to
  allow a more thorough check to be performed.
* Validates the [collection’s in-memory](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/collection.h)
  state with the [durable catalog](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/durable_catalog.h#L242-L243)
  entry information to ensure there are [no mismatches](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/catalog/collection_validation.cpp#L363-L425)
  between the two.
* [Initializes all the cursors](https://github.com/mongodb/mongo/blob/07765dda62d4709cddc9506ea378c0d711791b57/src/mongo/db/catalog/validate_state.cpp#L144-L205)
  on the `RecordStore` and `SortedDataInterface` of each index in the `ValidateState` object.
    + We choose a read timestamp (`ReadSource`) based on the validation mode and node configuration:
      |                |  Standalone  | Replica Set  |
      |----------------|:------------:|--------------|
      | **Foreground** | kNoTimestamp | kNoTimestamp |
      | **Background** | kNoTimestamp | kNoOverlap   |
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
|----------|--------------------|----------------------------------------------|
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

# Glossary
**binary comparable**: Two values are binary comparable if the lexicographical order over their byte
representation, from lower memory addresses to higher addresses, is the same as the defined ordering
for that type. For example, ASCII strings are binary comparable, but double precision floating point
numbers and little-endian integers are not.

**ident**: An ident is a unique identifier given to a storage engine resource. Collections and
indexes map application-layer names to storage engine idents. In WiredTiger, idents are implemented
as tables. For example, collection idents have the form: `collection-<counter>-<random number>`.

**oplog hole**: an uncommitted oplog write that can exist with out-of-order writes when a later
timestamped write happens to commit first. Oplog holes can exist in-memory and persisted on disk.

**oplogReadTimestamp**: the timestamp used for WT forward cursor oplog reads in order to avoid
advancing past oplog holes. Tracks in-memory oplog holes.

**oplogTruncateAfterPoint**: the timestamp after which oplog entries will be truncated during
startup recovery after an unclean shutdown. Tracks persisted oplog holes.

[`BSONObj::woCompare`]: https://github.com/mongodb/mongo/blob/v4.4/src/mongo/bson/bsonobj.h#L460
[`BSONElement::compareElements`]: https://github.com/mongodb/mongo/blob/v4.4/src/mongo/bson/bsonelement.cpp#L285
[`Ordering`]: https://github.com/mongodb/mongo/blob/v4.4/src/mongo/bson/ordering.h
[initial sync]: ../repl/README.md#initial-sync
