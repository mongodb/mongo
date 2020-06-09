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

## On-Disk Catalog
Discuss what the catalog looks like on disk -- e.g. just another WT data table we structure specially

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

# Index Builds
How do indexs work?

Read collection table, sort in-memory, write to index table.

## Hybrid Index Build
KeyString has its own section below. Index builds discussion may need to reference it

### Temporary Side Table For New Writes

### Temporary Table For Transient Conflicts

## Index Locks

## Multikey Indexes

## Cross-Replica Set Index Builds

# KeyString
describe how KeyString values are used for comparing and indexing BSON elements

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
    * If an [ident](#ident) is known to WiredTiger but is not present in the `_mdb_catalog`, [create a new collection](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/storage_engine_impl.cpp#L145-L189) with the prefix `local.orphan.<ident-name>` that references this ident.
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

## Purpose
‘an operations log’, entry for every write op, repl uses it, rollback, recovery, etc.

## Oplog Visibility

### Oplog ‘Holes’
because parallel writes

### Oplog Read Timestamp
only used for forward oplog cursors, backwards skips

## Oplog Truncation
capped collection or WT oplog stones

special timestamps we will not truncate with WT -- for which we delay truncation

new min oplog time retention, helps not fall off of the oplog

oplog durability considerations across nodes

# Glossary

## ident

And ident is a unique identifier given to a storage engine resource. Collections and indexes map application-layer names to storage engine idents. In WiredTiger, idents are implemented as tables. For example, collection idents have the form: `collection-<counter>-<random number>`.
